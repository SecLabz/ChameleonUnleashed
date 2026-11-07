import { test, expect } from '@playwright/test';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { physicalChooser } from './chooser.mjs';
import { startSimulation } from './pm3-simulation.mjs';

const run = promisify(execFile);
const deviceURL = 'http://192.168.16.1';
const uid = [0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6];
const version = [0, 4, 4, 2, 1, 0, 0x0f, 3];
const signature = Array.from({ length: 32 }, (_, index) => 0x60 + index);
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const writePayload = (page, before, after) => [
  8,
  7,
  ...uid,
  page,
  0,
  ...Array(16).fill(0),
  ...before,
  ...Array(12).fill(0),
  ...after,
  ...Array(12).fill(0)
];

// Native PM3 MFU header (56 bytes), then 45 NTAG213 pages, in 4K emulator RAM.
function fixture() {
  const bytes = Buffer.alloc(4096);
  bytes.set(version);
  bytes[11] = 44;
  bytes.set(signature, 12);
  for (let index = 0; index < 3; index++) {
    bytes.set([index + 1, index + 2, index + 3, 0xbd], 44 + index * 4);
  }
  const pages = Array.from({ length: 45 }, (_, page) => [page, page * 3 + 1, 255 - page, page ^ 0xa5]);
  pages[0] = [...uid.slice(0, 3), 0x88 ^ uid[0] ^ uid[1] ^ uid[2]];
  pages[1] = uid.slice(3);
  pages[2] = [uid[3] ^ uid[4] ^ uid[5] ^ uid[6], 0x48, 0, 0];
  pages[3] = [0xe1, 0x10, 0x12, 0];
  pages[40] = [0, 0, 0, 0];
  pages[41] = [0, 0, 0, 0xff]; // CFG0 byte 3 is AUTH0; FF disables password protection.
  pages[42] = [0, 0, 0, 0];
  pages[43] = [0xff, 0xff, 0xff, 0xff];
  pages[44] = [0x12, 0x34, 0, 0];
  bytes.set(pages.flat(), 56);
  return { bytes, pages };
}

async function type2Bench({ page, request }, testInfo, writeChanges = false) {
  const executable = process.env.CHAMELEON_PM3_CLIENT;
  const port = process.env.CHAMELEON_PM3_PORT;
  test.skip(!executable || !port, 'Requires an explicitly configured Proxmark3 HF fixture');
  const enabled = writeChanges ? 'CHAMELEON_BLE_TEST_TYPE2_WRITES' : 'CHAMELEON_BLE_TEST_TYPE2_READER';
  test.skip(process.env[enabled] !== '1', 'Requires explicit opt-in and an aligned HF simulation');
  test.setTimeout(180000);
  page.setDefaultTimeout(15000);
  await mkdir(testInfo.outputDir, { recursive: true });
  const pm3 = async (command, label) => {
    const result = await run(executable, [port, '-c', command], { timeout: 15000, maxBuffer: 4194304 });
    await writeFile(testInfo.outputPath(`${label}.log`), result.stdout + result.stderr);
  };
  const saveRam = async (label) => {
    const path = testInfo.outputPath(label);
    await pm3(`hf mf esave --4k -f ${JSON.stringify(path)}`, label);
    const bytes = await readFile(`${path}.bin`);
    expect(bytes.length).toBe(4096);
    return bytes;
  };
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const metadata = [];
  for (let slot = 0; slot < 8; slot++) {
    metadata.push(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body());
  }
  const originalRam = await saveRam('pm3-before');
  const restoreBin = testInfo.outputPath('pm3-before.bin');
  const prepared = fixture();
  const fixtureBin = testInfo.outputPath('fixture.bin');
  await writeFile(fixtureBin, prepared.bytes);
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  let stopSimulation;
  let session = 0;
  let qualified = false;
  let captured;
  let uiStatus;
  let cleanup;
  const writeEvidence = { enabled: writeChanges, usb: [], ble: [] };
  const usb = [];
  try {
    await pm3(`hf mf eload --4k -f ${JSON.stringify(fixtureBin)}`, 'pm3-load');
    expect(await saveRam('pm3-loaded')).toEqual(prepared.bytes);
    stopSimulation = await startSimulation(
      executable,
      port,
      'hf 14a sim -t 7 -u 04A1B2C3D4E5F6',
      testInfo.outputPath('simulation.log'),
      { restoreBin }
    );
    const openUsb = async () => {
      const opened = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
      expect(opened.status()).toBe(200);
      session = (await opened.json()).session;
    };
    const endUsb = async () => {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
      session = 0;
    };
    await openUsb();
    const command = async (number, payload, expected, status = 0) => {
      const response = await request.post(`${deviceURL}/api/command/${number}?id=${session}`, {
        data: Buffer.from(payload)
      });
      expect(response.status()).toBe(200);
      const reply = [...(await response.body())];
      usb.push({ command: number, payload, reply });
      await writeFile(testInfo.outputPath('usb-progress.json'), JSON.stringify(usb, null, 2));
      expect(reply).toEqual([status >> 8, status & 255, ...expected]);
    };
    const checkPublicMemory = async (pages) => {
      for (let start = 0; start <= 42; start += 8) {
        const end = Math.min(start + 7, 42);
        await command(2028, [5, start, 7, ...uid, end], pages.slice(start, end + 1).flat());
      }
    };
    await command(2000, [], [7, ...uid, 0x44, 0, 0, 0]);
    await command(2028, [0, 0, 7, ...uid], version);
    await checkPublicMemory(prepared.pages);
    await command(2028, [3, 2, 7, ...uid], [3, 4, 5]);
    await command(2028, [2, 0, 7, ...uid], signature);
    if (writeChanges) {
      const changed = [0xa1, 0xb2, 0xc3, 0xd4];
      await command(2030, writePayload(6, prepared.pages[6], changed), changed);
      await command(2028, [5, 6, 7, ...uid, 6], changed);
      writeEvidence.usb.push({ page: 6, before: prepared.pages[6], after: changed, verified: true });
      await command(2030, writePayload(6, prepared.pages[6], [1, 2, 3, 4]), [], 0x75);
      await command(2028, [5, 6, 7, ...uid, 6], changed);
      writeEvidence.usb.push({ staleBeforeRejected: true, status: 0x75, preserved: true });
      await command(2030, writePayload(6, changed, prepared.pages[6]), prepared.pages[6]);
      await checkPublicMemory(prepared.pages);
      writeEvidence.usb.push({ page: 6, restored: true });
    }
    await endUsb();
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await page.locator('[data-view-target="scan"]').click();
    await page.locator('#scan').click();
    await expect(page.locator('#studio-document')).toBeVisible();
    await expect(page.locator('#stop')).toBeHidden({ timeout: 60000 });
    await expect(page.locator('#status')).toHaveText('Type 2 read finished');
    uiStatus = await page.locator('#status').textContent();
    await page.locator('#export-menu summary').click();
    const download = page.waitForEvent('download');
    await page.locator('#export-document').click();
    const stream = await (await download).createReadStream();
    const parts = [];
    for await (const part of stream) {
      parts.push(part);
    }
    const exported = Buffer.concat(parts);
    await writeFile(testInfo.outputPath('captured.json'), exported);
    captured = JSON.parse(exported.toString()).document;
    expect(captured).toMatchObject({ kind: 'type2', type: 1100, unitBytes: 4 });
    expect(captured.card).toMatchObject({ uid, atqa: [0, 0x44], sak: 0, ats: [] });
    // Geometry is reconstructed on import; the portable export stores type and page count.
    expect(captured.emulator.options.slice(3)).toEqual([0, 45]);
    expect(captured.memory).toEqual([...prepared.pages.slice(0, 43), null, null]);
    expect(captured.emulator.extra).toEqual([
      [3, 4, 5, 0],
      version.slice(0, 4),
      version.slice(4),
      ...Array.from({ length: 8 }, (_, page) => signature.slice(page * 4, page * 4 + 4))
    ]);
    if (writeChanges) {
      const memory = page.locator('#memory textarea');
      const write = page.locator('#write-changes');
      const originalText = await memory.inputValue();
      const changedPages = structuredClone(prepared.pages);
      changedPages[6] = [0x21, 0x43, 0x65, 0x87];
      changedPages[8] = [0xfe, 0xdc, 0xba, 0x98];
      const lines = originalText.split('\n');
      for (const index of [6, 8]) {
        lines[index] = Buffer.from(changedPages[index]).toString('hex');
      }
      await memory.fill(lines.join('\n'));
      await expect(write).toHaveText('Write changes (2)');
      await expect(write).toBeEnabled();
      const decide = async (accepted) => {
        await write.click();
        const dialog = page.getByRole('dialog', { name: 'Write page changes?', exact: true });
        await expect(dialog).toContainText('Tag 04A1B2C3D4E5F6 · 2 user pages: 6, 8.');
        await dialog
          .getByRole('button', { name: accepted ? 'Write changes' : 'Cancel', exact: true })
          .click();
        await expect(page.locator('#status')).toHaveText(
          accepted ? '2 pages written and verified' : 'Writing cancelled'
        );
        await expect(page.locator('#stop')).toBeHidden();
      };
      await decide(false);
      await expect(write).toBeEnabled();
      await openUsb();
      await checkPublicMemory(prepared.pages);
      await endUsb();
      writeEvidence.ble.push({ cancelled: true, allPublicPagesPreserved: true });
      await decide(true);
      await expect(write).toHaveText('Write changes (0)');
      await expect(write).toBeDisabled();
      await openUsb();
      await checkPublicMemory(changedPages);
      await endUsb();
      writeEvidence.ble.push({ pages: [6, 8], writtenAndVerified: true, independentUsbReadback: true });
      await memory.fill(originalText);
      await expect(write).toHaveText('Write changes (2)');
      await decide(true);
      await expect(write).toHaveText('Write changes (0)');
      await expect(write).toBeDisabled();
      await openUsb();
      await checkPublicMemory(prepared.pages);
      await endUsb();
      writeEvidence.ble.push({ pages: [6, 8], restored: true, allPublicPagesPreserved: true });
      const external = [0xab, 0xcd, 0xef, 0x01];
      await openUsb();
      await command(2030, writePayload(6, prepared.pages[6], external), external);
      await endUsb();
      await memory.fill(lines.join('\n'));
      await expect(write).toHaveText('Write changes (2)');
      await write.click();
      await expect(page.locator('#status')).toContainText(
        '0/2 pages written and verified. Page 6 changed since reading. No writes were sent.'
      );
      await expect(page.locator('#stop')).toBeHidden();
      await expect(page.getByRole('dialog')).toHaveCount(0);
      await expect(write).toHaveText('Write changes');
      await expect(write).toBeDisabled();
      const externallyChanged = structuredClone(prepared.pages);
      externallyChanged[6] = external;
      await openUsb();
      await checkPublicMemory(externallyChanged);
      await command(2030, writePayload(6, external, prepared.pages[6]), prepared.pages[6]);
      await checkPublicMemory(prepared.pages);
      await endUsb();
      writeEvidence.ble.push({
        staleCaptureRejected: true,
        noUiWritesObserved: true,
        independentUsbReadback: true,
        readBindingInvalidated: true,
        fixtureRestoredThroughUsb: true
      });
      uiStatus = await page.locator('#status').textContent();
    }
    expect(leakedHttp).toEqual([]);
    checkChooser();
    qualified = true;
  } finally {
    // The radio stop must still run if browser or USB lease cleanup fails.
    try {
      await page.close();
      if (session) {
        expect(
          (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
        ).toBe(200);
      }
    } finally {
      try {
        if (stopSimulation) {
          await stopSimulation();
        }
      } finally {
        // Also cover a partial RAM load or startup failure before readiness.
        const current = await saveRam('pm3-after');
        if (!current.equals(originalRam)) {
          await pm3(`hf mf eload --4k -f ${JSON.stringify(restoreBin)}`, 'pm3-fallback-restore');
          expect(await saveRam('pm3-fallback-verified')).toEqual(originalRam);
          throw new Error('PM3 RAM needed fallback restoration; autonomous restoration did not qualify');
        }
      }
    }
    await pm3('trace list -t 14a -r -u', 'pm3-rf-trace');
    cleanup = JSON.parse(await readFile(testInfo.outputPath('simulation.log.cleanup.json'), 'utf8'));
    expect(cleanup).toMatchObject({ stopped: true, ramRestored: true, ramSha256: hash(originalRam) });
    await expect.poll(async () => (await info()).busy).toBe(false);
    const lease = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
    expect(lease.status()).toBe(200);
    const negativeSession = (await lease.json()).session;
    try {
      const absent = await request.post(`${deviceURL}/api/command/2000?id=${negativeSession}`, {
        data: Buffer.alloc(0)
      });
      expect(absent.status()).toBe(200);
      expect([...(await absent.body())]).toEqual([0, 1]);
    } finally {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${negativeSession}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    const after = await info();
    for (const key of ['chip', 'git', 'settings', 'reader', 'active_slot']) {
      expect(after[key], key).toEqual(before[key]);
    }
    for (let slot = 0; slot < 8; slot++) {
      expect(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body()).toEqual(metadata[slot]);
    }
    await testInfo.attach('physical-reader-type2-evidence', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          usb,
          card: captured?.card,
          type: captured?.type,
          uiStatus,
          publicPagesCompared: qualified ? 43 : 0,
          credentialPagesUnknown: [43, 44],
          signatureSynthetic: true,
          pm3Cleanup: cleanup,
          pm3RamSha256: hash(originalRam),
          simulationAbsentAfterStop: true,
          httpFallbackRequests: leakedHttp,
          writesToPm3EmulatorRam: writeEvidence,
          physicalTagWrites: false,
          fixture: 'PM3 NTAG213 simulation with synthetic public memory, counter and signature'
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
}

test('the real BLE UI captures NTAG213 public memory and metadata identically to USB', ({
  page,
  request
}, info) => type2Bench({ page, request }, info));

test('the real BLE UI cancels, writes, verifies and restores NTAG213 simulated user pages', ({
  page,
  request
}, info) => type2Bench({ page, request }, info, true));
