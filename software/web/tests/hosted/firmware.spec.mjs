import { createHash } from 'node:crypto';
import { test, expect } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../../..', import.meta.url));
const fixtures = JSON.parse(
  execFileSync(
    'python3',
    [
      '-c',
      `
import sys,json,base64
sys.path.insert(0,'resource/tools')
from test_web_update import fixtures,archive
f=fixtures()
print(json.dumps({name:{'zip':base64.b64encode(archive(*f[name],*(f['system'] if name=='legacy' else (None,None)))).decode(),'sizes':([len(f['system'][0])] if name=='legacy' else [])+[len(f[name][0])]} for name in ['normal','legacy']}))
`
    ],
    { cwd: root, encoding: 'utf8' }
  )
);

async function dfu(page, sizes, failCrc = false, fast = false) {
  await page.addInitScript(
    ({ sizes, failCrc, fast }) => {
      window.dfuTest = { choices: 0, activated: [], packetBytes: 0 };
      const crc32 = (bytes) => {
        let crc = 0xffffffff;
        for (const byte of bytes) {
          crc ^= byte;
          for (let bit = 0; bit < 8; bit++) {
            crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
          }
        }
        return (crc ^ 0xffffffff) >>> 0;
      };
      const u32 = (value) => {
        const bytes = new Uint8Array(4);
        new DataView(bytes.buffer).setUint32(0, value, true);
        return [...bytes];
      };
      Object.defineProperty(navigator, 'bluetooth', {
        value: {
          async requestDevice(options) {
            if (options.filters[0].services[0] !== 0xfe59 || options.filters[0].namePrefix !== 'CU-') {
              throw new Error('Expected Nordic DFU service');
            }
            const choice = window.dfuTest.choices++;
            const device = new EventTarget();
            const control = new EventTarget();
            let kind;
            let prn = 0;
            let remaining = 0;
            const data = { 1: [], 2: [] };
            control.startNotifications = async () => control;
            device.gatt = {
              connected: true,
              disconnect() {
                this.connected = false;
                device.dispatchEvent(new Event('gattserverdisconnected'));
              },
              async connect() {
                return this;
              },
              async getPrimaryService() {
                return {
                  async getCharacteristic(uuid) {
                    return uuid.includes('0001-') ? control : packet;
                  }
                };
              }
            };
            const packet = {
              async writeValueWithoutResponse(bytes) {
                if (bytes.length > (fast ? 244 : 20)) {
                  throw new DOMException('Packet exceeds default ATT MTU', 'InvalidModificationError');
                }
                data[kind].push(...bytes);
                window.dfuTest.packetBytes += bytes.length;
                if (prn && --remaining === 0) {
                  remaining = prn;
                  control.value = new DataView(
                    Uint8Array.of(
                      0x60,
                      3,
                      1,
                      ...u32(data[kind].length),
                      ...u32(crc32(data[kind]) ^ (failCrc && kind === 2 ? 1 : 0))
                    ).buffer
                  );
                  control.dispatchEvent(new Event('characteristicvaluechanged'));
                }
              }
            };
            control.writeValueWithResponse = async (bytes) => {
              let payload = [];
              if (bytes[0] === 11 && fast) {
                payload = [2, ...u32(0x43550002), ...u32(0xf3000), ...u32(0xb000)];
              } else if (bytes[0] === 2) {
                prn = bytes[1] + bytes[2] * 256;
                remaining = prn;
              } else if (bytes[0] === 6) {
                payload = [...u32(bytes[1] === 1 ? 512 : 4096), ...u32(0), ...u32(0)];
              } else if (bytes[0] === 1) {
                kind = bytes[1];
                if (kind === 1) {
                  data[1] = [];
                  data[2] = [];
                }
                remaining = prn;
              } else if (bytes[0] === 3) {
                payload = [
                  ...u32(data[kind].length),
                  ...u32(crc32(data[kind]) ^ (failCrc && kind === 2 ? 1 : 0))
                ];
              } else if (bytes[0] === 4 && kind === 2) {
                window.dfuTest.activated.push(data[2].length);
              }
              control.value = new DataView(Uint8Array.of(0x60, bytes[0], 1, ...payload).buffer);
              control.dispatchEvent(new Event('characteristicvaluechanged'));
              if (bytes[0] === 4 && kind === 2 && data[2].length === sizes[choice]) {
                queueMicrotask(() => device.gatt.disconnect());
              }
            };
            return device;
          }
        }
      });
    },
    { sizes, failCrc, fast }
  );
}

async function prepare(page, name) {
  await page.goto('./#device');
  await page.locator('#firmware-update-link').click();
  const modal = page.getByRole('dialog', { name: 'Firmware update', exact: true });
  await expect(modal).toBeVisible();
  await modal.getByLabel('Nordic firmware ZIP').setInputFiles({
    name: name + '.zip',
    mimeType: 'application/zip',
    buffer: Buffer.from(fixtures[name].zip, 'base64')
  });
  await modal.getByRole('button', { name: 'Prepare update', exact: true }).click();
  const confirm = page.getByRole('dialog', { name: 'Install firmware?', exact: true });
  await expect(confirm).toBeVisible();
  return { modal, confirm };
}

test('a hosted BLE updater transfers an application and waits for the bootloader reset', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await dfu(page, fixtures.normal.sizes, false, true);
  const { modal, confirm } = await prepare(page, 'normal');
  await confirm.getByRole('button', { name: 'Continue', exact: true }).click();
  await modal.getByRole('button', { name: 'Connect DFU and install', exact: true }).click();
  await expect(modal.getByRole('status')).toContainText('Firmware accepted. Reconnect your Ultra');
  expect(await page.evaluate(() => window.dfuTest.choices)).toBe(1);
  await modal.getByRole('button', { name: 'Close', exact: true }).click();
  await expect(modal).toHaveCount(0);
  expect(errors).toEqual([]);
});

test('restoring Official requires confirmation and a separate DFU connection for each image', async ({
  page
}) => {
  await dfu(page, fixtures.legacy.sizes);
  const { modal, confirm } = await prepare(page, 'legacy');
  await expect(confirm).toContainText('removes web recovery');
  await expect(confirm).toContainText('Export saved tags first');
  await confirm.getByRole('button', { name: 'Continue', exact: true }).click();
  await modal.getByRole('button', { name: 'Connect DFU and install', exact: true }).click();
  await expect(modal.getByRole('status')).toContainText('System firmware accepted');
  await modal.getByRole('button', { name: 'Connect DFU and install', exact: true }).click();
  await expect(modal.getByRole('status')).toContainText(
    'Firmware accepted. Reconnect using its application.'
  );
  expect(await page.evaluate(() => window.dfuTest.choices)).toBe(2);
});

test('a CRC mismatch stops installation and never claims success', async ({ page }) => {
  await dfu(page, fixtures.normal.sizes, true);
  const { modal, confirm } = await prepare(page, 'normal');
  await confirm.getByRole('button', { name: 'Continue', exact: true }).click();
  await modal.getByRole('button', { name: 'Connect DFU and install', exact: true }).click();
  await expect(modal.getByRole('status')).toContainText('CRC mismatch');
  expect(await page.evaluate(() => window.dfuTest.activated)).toEqual([]);
});

test('cancelling confirmation leaves the Ultra untouched', async ({ page }) => {
  await dfu(page, fixtures.normal.sizes);
  const { modal, confirm } = await prepare(page, 'normal');
  await confirm.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(modal.getByRole('button', { name: 'Prepare update', exact: true })).toBeEnabled();
  expect(await page.evaluate(() => window.dfuTest.choices)).toBe(0);
});

test('mobile catalogue selection downloads the dev ZIP from the shared Pages prefix', async ({
  page
}, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await dfu(page, fixtures.normal.sizes);
  const bytes = Buffer.from(fixtures.normal.zip, 'base64');
  const hash = createHash('sha256').update(bytes).digest('hex');
  const asset = { path: 'files/' + hash + '.zip', size: bytes.length, sha256: hash };
  await page.route(/\/updates\/(?:dev\/)?catalog\.json$/, (route) =>
    route.fulfill({
      json: {
        format: 1,
        sources: [
          {
            id: 'unleashed',
            label: 'Unleashed',
            releases: route.request().url().includes('/dev/')
              ? [{ version: 'v1.0.0-dev.0573e5b', layout: 2, packages: { application: asset } }]
              : []
          }
        ]
      }
    })
  );
  const downloads = [];
  await page.route('**/updates/dev/files/*.zip', async (route) => {
    downloads.push(route.request().url());
    await route.fulfill({ body: bytes, contentType: 'application/zip' });
  });
  await page.goto('./#device');
  await page.locator('#firmware-update-link').click();
  const modal = page.getByRole('dialog', { name: 'Firmware update', exact: true });
  await modal.getByLabel('Channel', { exact: true }).selectOption('development');
  await expect(modal.getByLabel('Version', { exact: true })).toHaveText('v1.0.0-dev.0573e5b');
  expect(await modal.evaluate((node) => node.scrollWidth <= node.clientWidth)).toBe(true);
  await page.screenshot({ path: testInfo.outputPath('ble-update-mobile.png') });
  await modal.getByRole('button', { name: 'Prepare update', exact: true }).click();
  await page
    .getByRole('dialog', { name: 'Install firmware?', exact: true })
    .getByRole('button', { name: 'Continue', exact: true })
    .click();
  await modal.getByRole('button', { name: 'Connect DFU and install', exact: true }).click();
  await expect(modal.getByRole('status')).toContainText('Firmware accepted');
  expect(downloads).toHaveLength(1);
  expect(downloads[0]).toContain('/project/ChameleonUnleashed/updates/dev/files/');
});

for (const state of [
  { busy: true, layout: 2 },
  { busy: false, layout: 0 }
]) {
  test(`an active app with incompatible update state is not restarted (${JSON.stringify(state)})`, async ({
    page
  }) => {
    await dfu(page, fixtures.normal.sizes);
    await page.goto('./#device');
    await page.evaluate(async (state) => {
      const { bleFirmwareUpdate } = await import(new URL('ble-update.mjs', document.baseURI).href);
      window.enteredDfu = 0;
      void bleFirmwareUpdate({
        transport: {
          closed: false,
          async request(path) {
            return path === '/api/device' ? { busy: state.busy } : { layout: state.layout };
          },
          async enterDfu() {
            window.enteredDfu++;
          }
        }
      });
    }, state);
    const modal = page.getByRole('dialog', { name: 'Firmware update', exact: true });
    await modal.getByLabel('Nordic firmware ZIP').setInputFiles({
      name: 'app.zip',
      mimeType: 'application/zip',
      buffer: Buffer.from(fixtures.normal.zip, 'base64')
    });
    await modal.getByRole('button', { name: 'Prepare update', exact: true }).click();
    await expect(modal.getByRole('status')).toContainText(
      state.busy ? 'Finish the active device operation' : 'Install the complete Unleashed CLI kit'
    );
    expect(await page.evaluate(() => window.enteredDfu)).toBe(0);
    expect(await page.evaluate(() => window.dfuTest.choices)).toBe(0);
    await expect(page.getByRole('dialog', { name: 'Install firmware?', exact: true })).toHaveCount(0);
  });
}
