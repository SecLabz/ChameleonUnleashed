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

async function dfu(page, sizes, { failCrc = false, fast = false, cancelFirst = false } = {}) {
  await page.addInitScript(
    ({ sizes, failCrc, fast, cancelFirst }) => {
      window.dfuTest = { choices: 0, prompts: 0, activated: [], packetBytes: 0 };
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
            if (cancelFirst && window.dfuTest.prompts++ === 0) {
              throw new DOMException('User cancelled the requestDevice() chooser.', 'NotFoundError');
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
    { sizes, failCrc, fast, cancelFirst }
  );
}

const zip = (name) => ({
  name: name + '.zip',
  mimeType: 'application/zip',
  buffer: Buffer.from(fixtures[name].zip, 'base64')
});

async function prepare(page, name, confirmTitle = 'Install firmware?') {
  await page.goto('./#device');
  await page.locator('#firmware-update-link').click();
  const modal = page.getByRole('dialog', { name: 'Firmware update', exact: true });
  await expect(modal).toBeVisible();
  await modal.getByLabel('Nordic firmware ZIP').setInputFiles(zip(name));
  await modal.getByRole('button', { name: 'Install selected ZIP', exact: true }).click();
  const confirm = page.getByRole('dialog', { name: confirmTitle, exact: true });
  await expect(confirm).toBeVisible();
  return { modal, confirm };
}

// Mounts the sheet directly, as the app does, with a stubbed reconnection.
async function sheet(page, { transport, version, deferReconnect = false } = {}) {
  await page.goto('./#device');
  await page.evaluate(
    async ({ transport, version, deferReconnect }) => {
      const { bleFirmwareUpdate } = await import(new URL('ble-update.mjs', document.baseURI).href);
      window.enteredDfu = 0;
      window.reconnects = 0;
      window.sheetResult = bleFirmwareUpdate({
        transport: transport && {
          closed: false,
          async request(path) {
            return path === '/api/device'
              ? { busy: transport.busy }
              : { layout: transport.layout, version: 1 };
          },
          async enterDfu() {
            window.enteredDfu++;
          }
        },
        reconnect: async (signal) => {
          window.reconnects++;
          if (deferReconnect) {
            window.verifySignal = signal;
            await new Promise((resolve) => {
              window.finishReconnect = resolve;
            });
          }
          window.verificationRequests = 0;
          return {
            request: async () => {
              window.verificationRequests++;
              return { version: window.reconnects === 1 ? version - 1 : version };
            }
          };
        }
      });
    },
    { transport, version, deferReconnect }
  );
  const modal = page.getByRole('dialog', { name: 'Firmware update', exact: true });
  await expect(modal).toBeVisible();
  return modal;
}

test('a hosted BLE update matches the USB page and asks once per Bluetooth chooser', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await dfu(page, fixtures.normal.sizes, { fast: true });
  const { modal, confirm } = await prepare(page, 'normal');
  await expect(confirm).toContainText('replaces web recovery');
  await confirm.getByRole('button', { name: 'Install', exact: true }).click();
  await expect(modal.getByRole('button', { name: 'Install selected ZIP' })).toBeHidden();
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  await expect(modal.getByRole('button', { name: 'Reconnect and verify', exact: true })).toBeEnabled();
  await expect(modal.locator('#fw-message')).toContainText('Firmware transferred');
  expect(await page.evaluate(() => window.dfuTest.choices)).toBe(1);
  await modal.getByRole('button', { name: 'Return to Chameleon', exact: true }).click();
  await expect(modal).toHaveCount(0);
  await expect(page.locator('#toast')).toContainText('Reconnect Bluetooth to verify');
  expect(errors).toEqual([]);
});

test('the sheet reconnects and verifies the installed version before reporting success', async ({ page }) => {
  await dfu(page, fixtures.normal.sizes);
  const modal = await sheet(page, { version: 0x1000001 });
  await expect(modal.locator('#fw-device')).toHaveText('Ultra not connected');
  await modal.getByLabel('Nordic firmware ZIP').setInputFiles(zip('normal'));
  await modal.getByRole('button', { name: 'Install selected ZIP', exact: true }).click();
  await page
    .getByRole('dialog', { name: 'Install firmware?', exact: true })
    .getByRole('button', { name: 'Install', exact: true })
    .click();
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  const verify = modal.getByRole('button', { name: 'Reconnect and verify', exact: true });
  for (const control of await modal.locator('select,input').all()) {
    await expect(control).toBeDisabled();
  }
  await verify.click();
  await expect(modal.locator('#fw-message')).toContainText('unexpected firmware version');
  await expect(modal.locator('#fw-title')).toBeEmpty();
  await verify.click();
  await expect(modal.locator('#fw-title')).toHaveText('Your Ultra is up to date');
  await expect(modal.locator('#fw-device')).toHaveText('Ultra connected · v1.0.1');
  await expect(verify).toBeHidden();
  await modal.getByRole('button', { name: 'Return to Chameleon', exact: true }).click();
  expect(await page.evaluate(() => window.sheetResult)).toBe(false);
});

test('closing the sheet cancels a pending verification without using a late connection', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await dfu(page, fixtures.normal.sizes);
  const modal = await sheet(page, { version: 0x1000001, deferReconnect: true });
  await modal.getByLabel('Nordic firmware ZIP').setInputFiles(zip('normal'));
  await modal.getByRole('button', { name: 'Install selected ZIP', exact: true }).click();
  await page
    .getByRole('dialog', { name: 'Install firmware?', exact: true })
    .getByRole('button', { name: 'Install', exact: true })
    .click();
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  await modal.getByRole('button', { name: 'Reconnect and verify', exact: true }).click();
  await modal.getByRole('button', { name: 'Return to Chameleon', exact: true }).click();
  await expect(modal).toHaveCount(0);
  expect(await page.evaluate(() => window.sheetResult)).toBe(true);
  expect(await page.evaluate(() => window.verifySignal.aborted)).toBe(true);
  await page.evaluate(async () => {
    window.finishReconnect();
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
  expect(await page.evaluate(() => window.verificationRequests)).toBe(0);
  expect(errors).toEqual([]);
});

test('restoring Official requires confirmation and a separate DFU connection for each image', async ({
  page
}) => {
  await dfu(page, fixtures.legacy.sizes);
  const { modal, confirm } = await prepare(page, 'legacy', 'Install complete firmware?');
  await expect(modal.locator('#fw-transition')).toBeVisible();
  await expect(confirm).toContainText('removes web recovery');
  await expect(confirm).toContainText('Export saved tags first');
  await confirm.getByRole('button', { name: 'Install', exact: true }).click();
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  await expect(modal.locator('#fw-message')).toContainText('System firmware installed');
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  await expect(modal.locator('#fw-title')).toHaveText('Firmware transferred and verified');
  expect(await page.evaluate(() => window.dfuTest.choices)).toBe(2);
  await expect(modal.getByRole('button', { name: 'Reconnect and verify' })).toBeHidden();
  await modal.getByRole('button', { name: 'Return to Chameleon', exact: true }).click();
  await expect(modal).toHaveCount(0);
});

test('a CRC mismatch stops installation and never claims success', async ({ page }) => {
  await dfu(page, fixtures.normal.sizes, { failCrc: true });
  const { modal, confirm } = await prepare(page, 'normal');
  await confirm.getByRole('button', { name: 'Install', exact: true }).click();
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  await expect(modal.locator('#fw-message')).toContainText('CRC mismatch');
  await expect(modal.locator('#fw-title')).toBeEmpty();
  await expect(modal.getByRole('button', { name: 'Install selected ZIP', exact: true })).toBeEnabled();
  expect(await page.evaluate(() => window.dfuTest.activated)).toEqual([]);
});

test('cancelling confirmation leaves the Ultra untouched', async ({ page }) => {
  await dfu(page, fixtures.normal.sizes);
  const { modal, confirm } = await prepare(page, 'normal');
  await confirm.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(modal.locator('#fw-message')).toHaveText('Update cancelled. Nothing installed.');
  await expect(modal.getByRole('button', { name: 'Install selected ZIP', exact: true })).toBeEnabled();
  expect(await page.evaluate(() => window.dfuTest.choices)).toBe(0);
});

test('a dismissed chooser asks again; Escape cannot interrupt and Stop explains how to resume', async ({
  page
}) => {
  await dfu(page, fixtures.normal.sizes, { cancelFirst: true });
  const { modal, confirm } = await prepare(page, 'normal');
  await confirm.getByRole('button', { name: 'Install', exact: true }).click();
  const select = modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true });
  await select.click();
  await expect(select).toBeEnabled();
  expect(await page.evaluate(() => window.dfuTest.prompts)).toBe(1);
  await page.keyboard.press('Escape');
  await expect(modal).toBeVisible();
  await expect(modal.getByRole('button', { name: 'Return to Chameleon' })).toBeHidden();
  await modal.getByRole('button', { name: 'Stop', exact: true }).click();
  await expect(modal.locator('#fw-message')).toContainText('Install again and select your Ultra in DFU');
  await expect(select).toBeHidden();
  await page.keyboard.press('Escape');
  await expect(modal).toHaveCount(0);
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
              ? [
                  {
                    version: 'v1.0.0-dev.0573e5b',
                    prerelease: true,
                    layout: 2,
                    packages: { application: asset }
                  }
                ]
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
  await modal.getByLabel('Update channel').selectOption('development');
  await expect(modal.getByLabel('Firmware version')).toHaveValue('v1.0.0-dev.0573e5b');
  await expect(modal.getByLabel('Firmware version').locator('option')).toHaveText('v1.0.0 · 0573e5b');
  expect(await modal.evaluate((node) => node.scrollWidth <= node.clientWidth)).toBe(true);
  await page.screenshot({ path: testInfo.outputPath('ble-update-mobile.png') });
  await modal.getByRole('button', { name: 'Install update', exact: true }).click();
  await page
    .getByRole('dialog', { name: 'Install firmware?', exact: true })
    .getByRole('button', { name: 'Install', exact: true })
    .click();
  await modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true }).click();
  await expect(modal.locator('#fw-message')).toContainText('Firmware transferred');
  await page.screenshot({ path: testInfo.outputPath('ble-update-verify.png') });
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
    const modal = await sheet(page, { transport: state });
    await expect(modal.locator('#fw-device')).toHaveText('Ultra connected · v0.0.1');
    await modal.getByLabel('Nordic firmware ZIP').setInputFiles(zip('normal'));
    await modal.getByRole('button', { name: 'Install selected ZIP', exact: true }).click();
    await expect(modal.locator('#fw-message')).toContainText(
      state.busy ? 'Finish the active device operation' : 'Install the complete Unleashed CLI kit'
    );
    expect(await page.evaluate(() => window.enteredDfu)).toBe(0);
    expect(await page.evaluate(() => window.dfuTest.choices)).toBe(0);
    await expect(page.getByRole('dialog', { name: 'Install firmware?', exact: true })).toHaveCount(0);
  });
}
