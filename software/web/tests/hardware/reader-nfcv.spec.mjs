import { test, expect } from '@playwright/test';
import { createHash } from 'node:crypto';
import { decodeSnapshot } from '../../src/device/hf-reader.mjs';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { OPERATION } from '../../src/device/ble-protocol.mjs';

const deviceURL = 'http://192.168.16.1';

// A physical, unprotected ICODE SLIX (28 x 4 bytes), left on the antenna.
// No PM3 simulation, EEPROM write, password command or slot save is used.
test('the physical SLIX is read completely by the BLE UI and matches USB', async ({
  page,
  request
}, testInfo) => {
  const identity = process.env.CHAMELEON_BLE_TEST_SLIX_UID;
  test.skip(!identity, 'Requires explicit opt-in with the UID of a physical SLIX');
  expect(identity).toMatch(/^E00401[0-9A-F]{10}$/i);
  const uid = [...Buffer.from(identity, 'hex')];
  const card = { protocol: 'nfc_v', uid, blocks: 28, block_size: 4 };
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before).toMatchObject({ busy: false, reader: false });
  const metadata = [];
  for (let slot = 0; slot < 8; slot++) {
    metadata.push(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body());
  }
  let session = 0;
  let baseline;
  let captured;
  let qualified = false;
  let trace;
  const usb = [];
  const leakedHttp = [];
  const checkChooser = await physicalChooser(page);
  const saveTrace = await traceGatt(page);
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  try {
    const opened = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
    expect(opened.status()).toBe(200);
    session = (await opened.json()).session;
    const command = async (number, payload) => {
      const response = await request.post(`${deviceURL}/api/command/${number}?id=${session}`, {
        data: Buffer.from(payload)
      });
      expect(response.status()).toBe(200);
      const bytes = [...(await response.body())];
      usb.push({ command: number, payload, reply: bytes });
      expect(bytes.slice(0, 2)).toEqual([0, 0]);
      return Uint8Array.from(bytes.slice(2));
    };
    const inventory = await command(2300, []);
    expect([...inventory.slice(2, 10)].reverse()).toEqual(uid);
    baseline = decodeSnapshot(await command(2308, [...uid, 0, 28, 4]), card);
    expect(baseline.complete).toBe(true);
    expect(baseline.memory).toHaveLength(28);
    expect(baseline.memory.every((block) => block?.length === 4)).toBe(true);
    expect(
      (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
    ).toBe(200);
    session = 0;

    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await page.locator('[data-view-target="scan"]').click();
    await page.locator('#scan').click();
    await expect(page.locator('#studio-document')).toBeVisible();
    await expect(page.locator('#stop')).toBeHidden({ timeout: 60000 });
    await expect(page.locator('#status')).toHaveText('Read complete');
    await page.locator('#export-menu summary').click();
    const downloading = page.waitForEvent('download');
    await page.locator('#export-document').click();
    const stream = await (await downloading).createReadStream();
    const parts = [];
    for await (const part of stream) {
      parts.push(part);
    }
    const exported = Buffer.concat(parts);
    await testInfo.attach('captured-slix.json', { body: exported, contentType: 'application/json' });
    captured = JSON.parse(exported.toString()).document;
    expect(captured).toMatchObject({ kind: 'nfcv', type: 1501, unitBytes: 4, card });
    expect(captured.memory).toEqual(baseline.memory);
    expect(captured.locks).toEqual(baseline.locks);
    expect(leakedHttp).toEqual([]);
    checkChooser();
    trace = await saveTrace(testInfo.outputPath('gatt.json'));
    expect(trace.errors).toEqual([]);
    expect(trace.pending).toEqual([]);
    const commands = trace.requests.filter((entry) => entry.operation === OPERATION.COMMAND);
    expect(commands.some((entry) => entry.argument === 2308 && entry.rfStatus === 0)).toBe(true);
    expect(commands.every((entry) => [2000, 2304, 2305, 2300, 2301, 2308].includes(entry.argument))).toBe(
      true
    );
    qualified = true;
  } finally {
    await page.close();
    if (session) {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    await expect.poll(async () => (await info()).busy).toBe(false);
    const after = await info();
    for (const key of ['chip', 'git', 'settings', 'reader', 'active_slot']) {
      expect(after[key], key).toEqual(before[key]);
    }
    for (let slot = 0; slot < 8; slot++) {
      expect(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body()).toEqual(metadata[slot]);
    }
    await testInfo.attach('slix-reader-evidence', {
      body: JSON.stringify(
        {
          qualified,
          chip: before.chip,
          firmware: before.git,
          uid: identity,
          usb,
          blocks: captured?.memory.length,
          memorySha256:
            captured && createHash('sha256').update(Buffer.from(captured.memory.flat())).digest('hex'),
          httpFallbackRequests: leakedHttp,
          rfWrites: false,
          pm3Used: false,
          preserved: true,
          gattRequests: trace?.requests.length
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
