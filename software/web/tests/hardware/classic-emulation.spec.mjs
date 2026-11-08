import { test, expect } from '@playwright/test';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { resolve } from 'node:path';
import { mkdir, writeFile } from 'node:fs/promises';
import { decodeSlotImage } from '../../src/documents/slot-image.mjs';
import { physicalChooser } from './chooser.mjs';

const run = promisify(execFile);
const deviceURL = 'http://192.168.16.1';

test('a physical PM3 reads Classic 1K identity and memory during real BLE control', async ({
  page,
  request
}, testInfo) => {
  const executable = process.env.CHAMELEON_PM3_CLIENT;
  const port = process.env.CHAMELEON_PM3_PORT;
  test.skip(!executable || !port, 'Requires an explicitly configured Proxmark3 HF fixture');
  test.skip(process.env.CHAMELEON_BLE_TEST_CLASSIC_EMULATION !== '1', 'Temporarily selects Classic slot 1');
  test.setTimeout(300000);
  await mkdir(testInfo.outputDir, { recursive: true });
  const get = async (path) => {
    const response = await request.get(deviceURL + path);
    expect(response.status(), path).toBe(200);
    return response;
  };
  const post = async (path, data = Buffer.alloc(0)) => {
    const response = await request.post(deviceURL + path, { data });
    expect(response.status(), path).toBe(200);
    return response;
  };
  const info = async () => (await get('/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const metadata = [];
  for (let slot = 0; slot < 8; slot++) {
    metadata.push(await (await get(`/api/slot/${slot}`)).body());
  }
  expect(metadata[0].readUInt16BE(0)).toBe(1001);
  expect(metadata[0][4] & 1).toBe(1);
  const reserve = async (path) => (await (await post(path)).json()).session;
  const end = async (session) => post(`/api/end?id=${session}`);
  const snapshot = async () => {
    const session = await reserve('/api/slots/session');
    const images = [];
    try {
      for (let slot = 0; slot < 8; slot++) {
        for (const [sense, offset] of [
          [2, 0],
          [1, 2]
        ]) {
          if (!metadata[slot].readUInt16BE(offset)) {
            continue;
          }
          const opened = await (
            await post(`/api/slots/open?id=${session}`, Buffer.from([slot, sense]))
          ).body();
          expect(opened.length).toBe(6);
          const size = opened.readUInt16BE(0);
          const chunks = [];
          for (let start = 0; start < size; start += 512) {
            const count = Math.min(512, size - start);
            const payload = Buffer.alloc(4);
            payload.writeUInt16BE(start, 0);
            payload.writeUInt16BE(count, 2);
            const chunk = await (await post(`/api/slots/read?id=${session}`, payload)).body();
            expect(chunk.length).toBe(count);
            chunks.push(chunk);
          }
          images.push({ slot, sense, wire: Buffer.concat(chunks) });
        }
      }
    } finally {
      await end(session);
    }
    return images;
  };
  const originals = await snapshot();
  const document = decodeSlotImage(
    new Uint8Array(originals.find(({ slot, sense }) => slot === 0 && sense === 2).wire)
  );
  expect(document.memory).toHaveLength(64);
  const hex = (bytes) => Buffer.from(bytes).toString('hex').toUpperCase();
  const activateUSB = async (slot) => {
    const meta = await (await get(`/api/slot/${slot}`)).body();
    const session = await reserve('/api/slots/session');
    try {
      expect([
        ...(await (
          await post(
            `/api/slots/activate?id=${session}`,
            Buffer.concat([Buffer.from([slot]), meta.subarray(-8)])
          )
        ).body())
      ]).toEqual([0, 0x68]);
    } finally {
      await end(session);
    }
    expect((await info()).active_slot).toBe(slot);
  };
  const evidence = [];
  const rfFailures = [];
  const refreshEvidence = [];
  const pm3 = async (command, label) => {
    const start = Date.now();
    let code = 0;
    let output;
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
    await writeFile(testInfo.outputPath(`${label}.log`), output);
    const cleaned = output.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '');
    expect(cleaned).not.toMatch(/cannot communicate|Capabilities structure version.*not the one expected/i);
    evidence.push({ label, command, code, elapsedMs: Date.now() - start });
    return { code, output: cleaned };
  };
  const identity = (result) => {
    expect(result.code).toBe(0);
    const values = (label) =>
      [...result.output.matchAll(new RegExp(`^\\[\\+\\]\\s*${label}:\\s*((?:[0-9A-F]{2}\\s*)+)`, 'gim'))].map(
        (match) => match[1].replace(/\s/g, '').toUpperCase()
      );
    expect(values('UID')).toEqual([hex(document.card.uid)]);
    expect(values('ATQA')).toEqual([hex(document.card.atqa)]);
    expect(values('SAK')).toEqual([hex([document.card.sak])]);
  };
  const sector = (result, number) => {
    expect(result.code).toBe(0);
    const rows = [...result.output.matchAll(/^\[=\]\s*(\d+)\s*\|\s*((?:[0-9A-F]{2}\s*){16})\|/gim)];
    expect(rows.map((row) => Number(row[1]))).toEqual([0, 1, 2, 3].map((index) => number * 4 + index));
    for (const row of rows) {
      const index = Number(row[1]);
      const bytes = [...Buffer.from(row[2].replace(/\s/g, ''), 'hex')];
      // PM3 injects the supplied key A into its printed trailer. Compare RF
      // access bytes only there; do not claim the printed keys were read.
      expect(index % 4 === 3 ? bytes.slice(6, 10) : bytes, `block ${index}`).toEqual(
        index % 4 === 3 ? document.memory[index].slice(6, 10) : document.memory[index]
      );
    }
  };
  const readSectors = async (phase, refresh = false) => {
    for (let round = 0; round < 2; round++) {
      for (let number = 0; number < 16; number++) {
        const key = hex(document.memory[number * 4 + 3].slice(0, 6));
        const label = `${phase}-round-${round}-sector-${number}`;
        let finished = false;
        const reading = pm3(`hf mf rdsc -s ${number} -k ${key}`, label).finally(() => {
          finished = true;
        });
        let refreshes = 0;
        if (refresh) {
          // PM3 startup precedes the actual RF exchange. Keep traffic active
          // for the entire process instead of finishing one refresh too early.
          while (!finished) {
            await page.locator('#refresh-device').click();
            await expect(page.locator('#refresh-device')).toBeEnabled();
            refreshes++;
          }
          refreshEvidence.push({ label, refreshes });
        }
        const result = await reading;
        try {
          sector(result, number);
        } catch (error) {
          // Preserve every failure. Later rounds never erase a failed read.
          rfFailures.push({ phase, round, sector: number, label, error: error.message });
        }
        if (phase !== 'baseline') {
          await expect(page.locator('#connection-help')).toHaveText('Connected over Bluetooth');
        }
      }
    }
  };
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  let readerSession = 0;
  let qualified = false;
  try {
    await activateUSB(0);
    identity(await pm3('hf 14a reader', 'baseline-identity'));
    await readSectors('baseline');
    expect(rfFailures, 'A reproducible USB/reference RF control is required').toEqual([]);
    readerSession = await reserve('/api/session');
    expect((await info()).reader).toBe(true);
    const absent = await pm3('hf 14a reader', 'negative-control-emulation-off');
    // This PM3 client silently returns PM3_ESOFT (-10, exit 246) on an
    // unsuccessful selection; it does not print a "no tag" message.
    expect(absent.code).toBe(246);
    expect(absent.output).not.toMatch(/^\[\+\]\s*(UID|ATQA|SAK):/im);
    await end(readerSession);
    readerSession = 0;
    identity(await pm3('hf 14a reader', 'baseline-restored-after-negative-control'));
    await activateUSB(before.active_slot);

    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    if (before.active_slot !== 0) {
      await page.locator('#slot-0').getByRole('button', { name: 'Select slot', exact: true }).click();
    }
    await expect.poll(async () => (await info()).active_slot).toBe(0);
    await expect.poll(async () => (await info()).busy).toBe(false);
    await page.getByRole('button', { name: 'Device', exact: true }).click();
    identity(await pm3('hf 14a reader', 'ble-identity'));
    await readSectors('ble-idle');
    await readSectors('ble-refresh', true);
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    if (before.active_slot !== 0) {
      await page
        .locator(`#slot-${before.active_slot}`)
        .getByRole('button', { name: 'Select slot', exact: true })
        .click();
    }
    await expect.poll(async () => (await info()).active_slot).toBe(before.active_slot);
    expect(leakedHttp).toEqual([]);
    checkChooser();
    expect(rfFailures, 'Every RF failure remains a qualification failure').toEqual([]);
    qualified = true;
  } finally {
    await page.close();
    if (readerSession) {
      await end(readerSession);
    }
    await expect.poll(async () => (await info()).busy).toBe(false);
    if ((await info()).active_slot !== before.active_slot) {
      await activateUSB(before.active_slot);
    }
    const after = await info();
    for (const key of ['chip', 'git', 'reader', 'active_slot', 'settings']) {
      expect(after[key], key).toEqual(before[key]);
    }
    for (let slot = 0; slot < 8; slot++) {
      expect(await (await get(`/api/slot/${slot}`)).body()).toEqual(metadata[slot]);
    }
    expect(await snapshot()).toEqual(originals);
    await testInfo.attach('physical-classic-emulation-evidence', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          type: 1001,
          uid: hex(document.card.uid),
          sectors: 16,
          comparedDataBlocks: 48,
          comparedAccessByteGroups: 16,
          keyAAuthentication: true,
          keyBAuthentication: false,
          physicalWrites: false,
          usbPowered: true,
          httpFallbackRequests: leakedHttp,
          roundsPerPhase: 2,
          refreshEvidence,
          rfFailures,
          evidence
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
