import { test, expect } from '@playwright/test';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { resolve } from 'node:path';
import { mkdir, writeFile } from 'node:fs/promises';
import { decodeSlotImage } from '../../src/documents/slot-image.mjs';
import { physicalChooser } from './chooser.mjs';

const run = promisify(execFile);
const deviceURL = 'http://192.168.16.1';

test('a physical reader sees identical SLIX memory while Web Bluetooth stays connected', async ({
  page,
  request
}, testInfo) => {
  const executable = process.env.CHAMELEON_PM3_CLIENT;
  const port = process.env.CHAMELEON_PM3_PORT;
  test.skip(!executable || !port, 'Requires an explicitly configured Proxmark3 reader fixture');
  test.skip(
    process.env.CHAMELEON_BLE_TEST_UNSUPPORTED_EMULATION !== '1',
    'Diagnostic only: NFC-V emulation with BLE connected is outside the supported scope'
  );
  test.setTimeout(180000);
  await mkdir(testInfo.outputDir, { recursive: true });
  const before = await (await request.get(deviceURL + '/api/device')).json();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const slot = before.active_slot;
  const metadata = await (await request.get(`${deviceURL}/api/slot/${slot}`)).body();
  expect(metadata.readUInt16BE(0)).toBe(1500);

  async function reservation(path) {
    let token;
    await expect
      .poll(async () => {
        const response = await request.post(deviceURL + path, { data: Buffer.alloc(0) });
        if (response.status() === 200) {
          token = (await response.json()).session;
        } else {
          expect(response.status()).toBe(409);
        }
        return response.status();
      })
      .toBe(200);
    return token;
  }

  async function end(token) {
    expect((await request.post(`${deviceURL}/api/end?id=${token}`, { data: Buffer.alloc(0) })).status()).toBe(
      200
    );
  }

  async function snapshot() {
    const token = await reservation('/api/slots/session');
    try {
      const opened = await (
        await request.post(`${deviceURL}/api/slots/open?id=${token}`, {
          data: Buffer.from([slot, 2])
        })
      ).body();
      expect(opened.length).toBe(6);
      const size = opened.readUInt16BE(0);
      expect(size).toBeLessThanOrEqual(512);
      const body = Buffer.alloc(4);
      body.writeUInt16BE(size, 2);
      const response = await request.post(`${deviceURL}/api/slots/read?id=${token}`, { data: body });
      expect(response.status()).toBe(200);
      return new Uint8Array(await response.body());
    } finally {
      await end(token);
    }
  }

  const original = await snapshot();
  const document = decodeSlotImage(original);
  const uid = document.card.uid
    .map((byte) => byte.toString(16).padStart(2, '0'))
    .join('')
    .toUpperCase();
  const evidence = [];
  async function pm3(command, label) {
    const start = Date.now();
    let output;
    let code = 0;
    try {
      const result = await run(resolve(executable), [port, '-c', command], {
        cwd: testInfo.outputDir,
        timeout: 20000,
        maxBuffer: 4 * 1024 * 1024
      });
      output = result.stdout + result.stderr;
    } catch (error) {
      if (typeof error.code !== 'number' || error.killed) {
        throw error;
      }
      code = error.code;
      output = (error.stdout || '') + (error.stderr || '');
    }
    await writeFile(testInfo.outputPath(label + '.log'), output);
    const cleaned = output.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '');
    expect(cleaned).not.toMatch(/cannot communicate|Capabilities structure version.*not the one expected/i);
    evidence.push({ command, label, code, elapsedMs: Date.now() - start });
    return { output: cleaned, code };
  }

  function identity(result) {
    expect(result.code).toBe(0);
    const uids = [...result.output.matchAll(/^\[\+\]\s*UID\.+\s*((?:[0-9A-F]{2}\s*){8})/gim)];
    expect(uids).toHaveLength(1);
    expect(uids[0][1].replace(/\s/g, '').toUpperCase()).toBe(uid);
  }

  function memory(result) {
    expect(result.code).toBe(0);
    const rows = [
      ...result.output.matchAll(/^\[=\]\s*(\d+)\s*\|\s*((?:[0-9A-F]{2}\s*){4})\|\s*([01])\s*\|/gim)
    ];
    expect(rows).toHaveLength(document.memory.length);
    expect(rows.map((row) => Number(row[1]))).toEqual(document.memory.map((_, index) => index));
    for (const row of rows) {
      const index = Number(row[1]);
      expect([...Buffer.from(row[2].replace(/\s/g, ''), 'hex')], `block ${index}`).toEqual(
        document.memory[index]
      );
      expect(row[3] === '1', `block ${index} lock`).toBe(document.locks[index]);
    }
  }

  identity(await pm3('hf 15 info', 'baseline-identity'));
  memory(await pm3('hf 15 dump --ns', 'baseline-memory'));
  const token = await reservation('/api/session');
  try {
    const absent = await pm3('hf 15 info', 'negative-control-emulation-off');
    expect(absent.output).toMatch(/no tag found/i);
    expect(absent.output).not.toMatch(/\bUID\.{3,}/i);
  } finally {
    await end(token);
  }
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  let bluetooth = false;
  await page.route('**/api/**', (route) => {
    if (bluetooth) {
      leakedHttp.push(route.request().url());
    }
    return route.abort();
  });
  let qualified = false;
  try {
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await expect(page.locator('#connection-help')).toHaveText('Connected over Bluetooth');
    bluetooth = true;
    identity(await pm3('hf 15 info', 'ble-identity'));
    for (let cycle = 0; cycle < 3; cycle++) {
      const rfRead = pm3('hf 15 dump --ns', `ble-memory-${cycle + 1}`);
      await page.locator('#refresh-device').click();
      memory(await rfRead);
      await expect(page.locator('#refresh-device')).toBeEnabled();
      await expect(page.locator('#connection-help')).toHaveText('Connected over Bluetooth');
    }
    expect(leakedHttp).toEqual([]);
    checkChooser();
    qualified = true;
  } finally {
    bluetooth = false;
    await page.close();
    // A failed RF comparison must still verify disconnect cleanup and the
    // saved image. Keep its report explicitly separate from qualification.
    await expect
      .poll(async () => (await (await request.get(deviceURL + '/api/device')).json()).busy)
      .toBe(false);
    expect([...(await snapshot())]).toEqual([...original]);
    const after = await (await request.get(deviceURL + '/api/device')).json();
    for (const key of ['chip', 'reader', 'active_slot', 'settings']) {
      expect(after[key], key).toEqual(before[key]);
    }
    expect(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body()).toEqual(metadata);
    await writeFile(
      testInfo.outputPath('rf-emulation-evidence.json'),
      JSON.stringify(
        {
          qualified,
          preserved: true,
          physicalHardware: true,
          chip: after.chip,
          firmware: after.git,
          fixtureType: 1500,
          blocks: document.memory.length,
          usbPowered: true,
          physicalWrites: false,
          evidence
        },
        null,
        2
      )
    );
  }
});
