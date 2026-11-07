import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { ESSENTIALS } from '../../../../.cache/web/assets/modules/documents/classic-keys.mjs';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { startSimulation } from './pm3-simulation.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';
import { uid, keyA, access, classicFixture } from './classic-fixture.mjs';
import { decodeSectorDiagnostic } from './classic-sector-result.mjs';

const run = promisify(execFile);
const deviceURL = 'http://192.168.16.1';
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');

// Diagnostic completion is distinct from error-free RF qualification. Every
// firmware error/partial mask remains in the results; mutations are never sent.
test('characterize Classic RF errors across USB, BLE and connected-idle/load phases', async ({
  page,
  request
}, testInfo) => {
  const executable = process.env.CHAMELEON_PM3_CLIENT;
  const port = process.env.CHAMELEON_PM3_PORT;
  test.skip(
    process.env.CHAMELEON_BLE_TEST_CLASSIC_DIAGNOSTICS !== '1' || !executable || !port,
    'Requires explicit physical diagnostic opt-in and the stop-qualified PM3 fixture'
  );
  test.setTimeout(240000);
  page.setDefaultTimeout(15000);
  await mkdir(testInfo.outputDir, { recursive: true });
  const size = process.env.CHAMELEON_BLE_TEST_CLASSIC_DIAGNOSTICS_SIZE || '1k';
  expect(['1k', '2k']).toContain(size);
  const detailed = size === '2k';
  const simulationCommand = `hf mf sim --${size} -u 11223344 -i`;
  const classicStopReport = detailed ? process.env.CHAMELEON_PM3_CLASSIC_2K_STOP_REPORT : undefined;
  if (detailed) {
    expect(classicStopReport, 'Requires the exact 2K four-fault stop report').toBeTruthy();
    const check = await run(
      process.env.PYTHON || 'python3',
      [
        fileURLToPath(new URL('./pm3_classic_qualification.py', import.meta.url)),
        '--live',
        simulationCommand,
        classicStopReport,
        executable,
        port
      ],
      { timeout: 15000 }
    );
    await writeFile(testInfo.outputPath('pm3-identity-check.json'), check.stdout);
  }
  const pm3 = async (command, label) => {
    const result = await run(executable, [port, '-c', command], { timeout: 15000, maxBuffer: 4194304 });
    await writeFile(testInfo.outputPath(`${label}.log`), result.stdout + result.stderr);
  };
  const saveRam = async (label) => {
    const path = testInfo.outputPath(label);
    await pm3(`hf mf esave --4k -f ${JSON.stringify(path)}`, label);
    const bytes = await readFile(`${path}.bin`);
    expect(bytes).toHaveLength(4096);
    return bytes;
  };
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const snapshot = await usbSlotSnapshot(request, deviceURL);
  const originalRam = await saveRam('pm3-before');
  const restoreBin = testInfo.outputPath('pm3-before.bin');
  const fixture = classicFixture(size);
  const fixtureBin = testInfo.outputPath('fixture.bin');
  await writeFile(fixtureBin, fixture.bytes);
  const saveGattTrace = await traceGatt(page);
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  const bundle = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
      import { connectBluetooth } from './src/device/bluetooth.mjs';
      import { request, setDeviceTransport } from './src/device/transport.mjs';
      const bench = globalThis.bench = { request };
      document.querySelector('button').onclick = () => {
        bench.connecting = connectBluetooth().then(transport => {
          bench.transport = transport;
          setDeviceTransport(transport);
          return request('/api/device');
        });
      };
    `
    },
    bundle: true,
    format: 'esm',
    write: false
  });
  await page.route('**/rf-diagnostic', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<meta name="chameleon-hosted" content="1"><button>Connect diagnostic Ultra</button><script type="module" src="./rf-diagnostic.mjs"></script>'
    })
  );
  await page.route('**/rf-diagnostic.mjs', (route) =>
    route.fulfill({ contentType: 'text/javascript', body: bundle.outputFiles[0].text })
  );
  await page.goto('./rf-diagnostic');
  const records = [];
  const phases = [];
  const inspectPayload = [
    uid.length,
    ...uid,
    ...Array(6).fill(0),
    4,
    0,
    8,
    ESSENTIALS.length,
    ...ESSENTIALS.flat()
  ];
  let session = 0;
  let transport = 'usb';
  let stopSimulation;
  let cleanup;
  let gatt;
  let completed = false;
  let polling = false;
  let started;
  const raw = async (path, bytes) => {
    if (transport === 'ble') {
      return await page.evaluate(
        async ({ path, bytes }) => {
          const reply = await globalThis.bench.request(path, Uint8Array.from(bytes));
          return reply instanceof Uint8Array ? [...reply] : reply;
        },
        { path, bytes }
      );
    }
    const reply = await request.post(deviceURL + path, { data: Buffer.from(bytes), timeout: 10000 });
    expect(reply.status()).toBe(200);
    return reply.headers()['content-type']?.includes('json') ? await reply.json() : [...(await reply.body())];
  };
  const command = async (phase, cycle, number, payload) => {
    const at = Date.now();
    const reply = await raw(`/api/command/${number}?id=${session}`, payload);
    expect(Array.isArray(reply)).toBe(true);
    expect(reply.length).toBeGreaterThanOrEqual(2);
    const record = {
      phase,
      cycle,
      transport,
      command: number,
      payload,
      reply,
      rfStatus: reply[0] * 256 + reply[1],
      elapsedMs: Date.now() - at
    };
    records.push(record);
    await writeFile(testInfo.outputPath('rf-progress.json'), JSON.stringify(records, null, 2));
    return record;
  };
  const phase = async (name, useBle) => {
    transport = useBle ? 'ble' : 'usb';
    const start = records.length;
    const cycles = detailed ? 2 : 8;
    for (let cycle = 0; cycle < cycles; cycle++) {
      expect(
        Date.now() - started,
        'bounded diagnostic must finish before independent stop deadline'
      ).toBeLessThan(140000);
      session = (await raw('/api/session', [])).session;
      expect(session).toBeGreaterThan(0);
      try {
        const identity = await command(name, cycle, 2000, []);
        if (!identity.rfStatus) {
          expect(identity.reply).toEqual([0, 0, 4, ...uid, 4, 0, 8, 0]);
        }
        const inspection = await command(
          name,
          cycle,
          2040,
          detailed ? [...inspectPayload, 1] : inspectPayload
        );
        if (!inspection.rfStatus) {
          expect(inspection.reply).toHaveLength(detailed ? 29 : 13);
          if (detailed) {
            expect(inspection.reply.slice(13)).toEqual([1, 14, ...fixture.model.ats, 0x60, 0xd3]);
          }
        }
        for (const sectorIndex of detailed ? fixture.sectors.map((_, index) => index) : [6]) {
          expect(
            Date.now() - started,
            'Bounded diagnostic must stop before the independent deadline'
          ).toBeLessThan(140000);
          const sector = await command(name, cycle, 2027, [
            0x60,
            sectorIndex,
            ...keyA,
            uid.length,
            ...uid,
            ...(detailed ? [1] : [])
          ]);
          sector.sector = sectorIndex;
          if (detailed) {
            const { blocks, ...diagnostic } = decodeSectorDiagnostic(sector.reply, 4);
            Object.assign(sector, diagnostic);
            if (diagnostic.firstFailedOffset !== null) {
              sector.firstFailedBlock = fixture.sectors[sectorIndex].first + diagnostic.firstFailedOffset;
            }
            if (blocks) {
              for (const [offset, block] of blocks.entries()) {
                if (block) {
                  const expected =
                    offset === 3
                      ? [...Array(6).fill(0), ...access, ...Array(6).fill(0)]
                      : fixture.blocks[sectorIndex * 4 + offset];
                  expect(block).toEqual(expected);
                }
              }
            }
            // Keep every failed measurement. No automatic missing-block retries
            // in detailed mode; a later phase is an independent observation.
            await writeFile(testInfo.outputPath('rf-progress.json'), JSON.stringify(records, null, 2));
            continue;
          }
          if (!sector.rfStatus) {
            expect(sector.reply).toHaveLength(68);
            const mask = sector.reply[2] * 256 + sector.reply[3];
            expect(mask & ~15).toBe(0);
            sector.blockMask = mask;
            for (let offset = 0; offset < 4; offset++) {
              if (mask & (1 << offset)) {
                const expected =
                  offset === 3
                    ? [...Array(6).fill(0), ...access, ...Array(6).fill(0)]
                    : fixture.blocks[24 + offset];
                expect(sector.reply.slice(4 + offset * 16, 20 + offset * 16)).toEqual(expected);
              } else {
                const retry = await command(name, cycle, 2008, [0x60, 24 + offset, ...keyA]);
                retry.fallbackForPartialSector = true;
                if (!retry.rfStatus) {
                  const expected =
                    offset === 3
                      ? [...Array(6).fill(0), ...access, ...Array(6).fill(0)]
                      : fixture.blocks[24 + offset];
                  expect(retry.reply).toEqual([0, 0, ...expected]);
                }
              }
            }
          }
        }
      } finally {
        await raw(`/api/end?id=${session}`, []);
        session = 0;
      }
    }
    const slice = records.slice(start);
    phases.push({
      name,
      transport,
      cycles,
      commands: slice.length,
      rfErrors: slice
        .filter((r) => r.rfStatus)
        .map((r) => ({ command: r.command, status: r.rfStatus, cycle: r.cycle })),
      partialSectors: slice
        .filter((r) => r.blockMask !== undefined && r.blockMask !== null && r.blockMask !== 15)
        .map((r) => ({
          cycle: r.cycle,
          sector: r.sector,
          mask: r.blockMask,
          blockReadStatus: r.blockReadStatus,
          firstFailedBlock: r.firstFailedBlock
        }))
    });
    await writeFile(testInfo.outputPath('phase-progress.json'), JSON.stringify(phases, null, 2));
  };
  try {
    await pm3(`hf mf eload --4k -f ${JSON.stringify(fixtureBin)}`, 'pm3-load');
    expect(await saveRam('pm3-loaded')).toEqual(fixture.bytes);
    stopSimulation = await startSimulation(
      executable,
      port,
      simulationCommand,
      testInfo.outputPath('simulation.log'),
      { restoreBin, classicStop: true, classicStopReport }
    );
    started = Date.now();
    await phase('usb-before-ble', false);
    await page.getByRole('button', { name: 'Connect diagnostic Ultra', exact: true }).click();
    expect((await page.evaluate(() => globalThis.bench.connecting)).chip).toBe(before.chip);
    await phase('usb-ble-idle', false);
    await phase('ble-first', true);
    await page.evaluate(() => {
      const bench = globalThis.bench;
      bench.polling = true;
      bench.pollCount = 0;
      bench.pollWork = (async () => {
        while (bench.polling) {
          await bench.request('/api/device');
          bench.pollCount++;
        }
      })();
    });
    polling = true;
    await phase('usb-ble-refresh', false);
    const pollCount = await page.evaluate(async () => {
      globalThis.bench.polling = false;
      await globalThis.bench.pollWork;
      return globalThis.bench.pollCount;
    });
    polling = false;
    expect(pollCount).toBeGreaterThan(0);
    phases.at(-1).bleRefreshes = pollCount;
    await phase('ble-second', true);
    await page.evaluate(() => globalThis.bench.transport.disconnect());
    await phase('usb-after-ble', false);
    checkChooser();
    expect(leakedHttp).toEqual([]);
    completed = true;
  } finally {
    try {
      if (polling) {
        await page.evaluate(async () => {
          globalThis.bench.polling = false;
          await globalThis.bench.pollWork;
        });
      }
      if (session) {
        await raw(`/api/end?id=${session}`, []);
      }
    } finally {
      try {
        gatt = await saveGattTrace(testInfo.outputPath('gatt-trace.json')).catch(async (error) => {
          await writeFile(testInfo.outputPath('gatt-trace-error.txt'), error.stack || error.message);
        });
        await page.close();
      } finally {
        try {
          if (stopSimulation) {
            await stopSimulation();
          }
        } finally {
          const current = await saveRam('pm3-after');
          if (!current.equals(originalRam)) {
            await pm3(`hf mf eload --4k -f ${JSON.stringify(restoreBin)}`, 'pm3-fallback-restore');
            expect(await saveRam('pm3-fallback-verified')).toEqual(originalRam);
            throw new Error('PM3 needed fallback RAM restoration; autonomous cleanup did not qualify');
          }
        }
      }
    }
    await pm3('trace list -t 14a -r -u', 'pm3-rf-trace');
    cleanup = JSON.parse(await readFile(testInfo.outputPath('simulation.log.cleanup.json'), 'utf8'));
    expect(cleanup).toMatchObject({
      stopped: true,
      persistentStopPingConfirmed: true,
      ramRestored: true,
      ramSha256: hash(originalRam)
    });
    await expect.poll(async () => (await info()).busy).toBe(false);
    transport = 'usb';
    session = (await raw('/api/session', [])).session;
    try {
      const absent = await raw(`/api/command/2000?id=${session}`, []);
      expect(absent).toEqual([0, 1]);
    } finally {
      await raw(`/api/end?id=${session}`, []);
      session = 0;
    }
    const after = await info();
    for (const key of ['chip', 'git', 'settings', 'reader', 'active_slot']) {
      expect(after[key], key).toEqual(before[key]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(snapshot);
    if (completed) {
      expect(phases).toHaveLength(6);
      expect(gatt?.errors).toEqual([]);
      expect(gatt?.truncated).toBe(false);
    }
    const rfErrorFree = records.every(
      (r) => r.rfStatus === 0 && (r.blockMask === undefined || r.blockMask === 15)
    );
    const evidence = {
      completed,
      diagnosticOnly: true,
      rfErrorFree,
      fullRfReliabilityQualified: false,
      classicSize: size,
      classicStopReport,
      detailedSectorStatus: detailed,
      phases,
      records,
      gattOperations: gatt?.requests.length,
      gattFrameErrors: gatt?.errors,
      cleanup,
      preserved: true,
      simulationAbsentAfterStop: true,
      httpFallbackRequests: leakedHttp,
      chip: before.chip,
      firmware: before.git,
      physicalTagWrites: false
    };
    await writeFile(testInfo.outputPath('diagnostic.json'), JSON.stringify(evidence, null, 2));
    await testInfo.attach('physical-classic-rf-diagnostic', {
      body: JSON.stringify(evidence),
      contentType: 'application/json'
    });
  }
});
