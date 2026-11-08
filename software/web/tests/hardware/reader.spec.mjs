import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { physicalChooser } from './chooser.mjs';
import { ST25TB, encodeSt25tbTearoff, decodeSt25tbTearoff } from '../../src/device/st25tb-protocol.mjs';

const deviceURL = 'http://192.168.16.1';
const scans = [
  { command: 2000, payload: [], status: 1, family: 'NFC-A' },
  { command: 2304, payload: [], status: 1, family: 'NFC-B' },
  { command: 2300, payload: [], status: 1, family: 'NFC-V' },
  { command: 3000, payload: [], status: 0x41, family: 'EM410x' },
  { command: 3004, payload: [], status: 0x41, family: 'Viking' },
  { command: 3002, payload: [0], status: 0x41, family: 'HID Prox' },
  { command: 3010, payload: [0], status: 0x41, family: 'ioProx' },
  { command: 3014, payload: [], status: 0x41, family: 'PAC' },
  { command: 3019, payload: [], status: 0x41, family: 'Jablotron' }
];

// No tag or PM3 simulation may be in the field. This verifies error outcomes
// and reader cleanup, not successful RF decoding or physical tag writes.
test('physical BLE readers match USB errors, cancel an empty-field job and release their lease', async ({
  page,
  request
}, testInfo) => {
  test.skip(process.env.CHAMELEON_BLE_TEST_NO_TAG !== '1', 'Requires an explicitly empty RF fixture');
  test.setTimeout(180000);
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const metadata = [];
  for (let slot = 0; slot < 8; slot++) {
    metadata.push(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body());
  }
  const baseline = [];
  // Synthetic target: the engine must verify this identity before any write.
  const tearoffBody = encodeSt25tbTearoff([0xd0, 2, 0x33, 0, 0, 0, 0, 1], 5, 0x1000);
  let usbTearoff;
  const opened = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
  expect(opened.status()).toBe(200);
  const usbSession = (await opened.json()).session;
  try {
    for (const scan of scans) {
      const start = Date.now();
      const response = await request.post(`${deviceURL}/api/command/${scan.command}?id=${usbSession}`, {
        data: Buffer.from(scan.payload)
      });
      expect(response.status(), scan.family).toBe(200);
      const bytes = [...(await response.body())];
      expect(bytes, `The ${scan.family} control must have no tag`).toEqual([0, scan.status]);
      baseline.push({ ...scan, bytes, elapsedMs: Date.now() - start });
    }
    const jobPath = `${deviceURL}${ST25TB.TEAROFF_PATH}`;
    expect(
      (await request.post(`${jobPath}/start?id=${usbSession}`, { data: Buffer.from(tearoffBody) })).status()
    ).toBe(200);
    let state;
    const jobState = async () => {
      const response = await request.post(`${jobPath}/state?id=${usbSession}`, { data: Buffer.alloc(0) });
      expect(response.status()).toBe(200);
      state = decodeSt25tbTearoff(new Uint8Array(await response.body()));
      return state.state;
    };
    await expect.poll(jobState).toBe(2);
    expect(state.attempts).toBe(0);
    const cancelStarted = Date.now();
    expect(
      (await request.post(`${jobPath}/cancel?id=${usbSession}`, { data: Buffer.alloc(0) })).status()
    ).toBe(200);
    await expect.poll(jobState).toBe(ST25TB.DONE);
    usbTearoff = { ...state, cancelElapsedMs: Date.now() - cancelStarted };
    expect(usbTearoff).toMatchObject({ outcome: 1, attempts: 0, flags: ST25TB.CLEANUP_COMPLETE });
    expect(usbTearoff.cancelElapsedMs).toBeLessThan(5000);
  } finally {
    expect(
      (await request.post(`${deviceURL}/api/end?id=${usbSession}`, { data: Buffer.alloc(0) })).status()
    ).toBe(200);
  }

  const bundle = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
        import { connectBluetooth } from './src/device/bluetooth.mjs';
        import { request, setDeviceTransport } from './src/device/transport.mjs';
        import { Device } from './src/device/device.mjs';
        import { ST25TB, decodeSt25tbTearoff } from './src/device/st25tb-protocol.mjs';
        const bench = globalThis.bench = { request, Device, ST25TB, decodeSt25tbTearoff };
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
  const leakedHttp = [];
  page.on('request', (request) => {
    if (new URL(request.url()).pathname.startsWith('/api/')) {
      leakedHttp.push(request.url());
    }
  });
  await page.route('**/reader-bench', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<button>Connect test Ultra</button><script type="module" src="./reader-bench.mjs"></script>'
    })
  );
  await page.route('**/reader-bench.mjs', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: bundle.outputFiles[0].text
    })
  );
  const checkChooser = await physicalChooser(page);
  let results = [];
  let bleTearoff;
  let disconnectedDuringTearoff = false;
  let qualified = false;
  try {
    await page.goto('/reader-bench');
    await page.getByRole('button', { name: 'Connect test Ultra' }).click();
    expect((await page.evaluate(() => globalThis.bench.connecting)).chip).toBe(before.chip);
    const session = await page.evaluate(async () => {
      const { bench } = globalThis;
      bench.reader = new bench.Device();
      await bench.reader.reserve();
      return bench.reader.session;
    });
    const owned = await info();
    expect(owned.busy).toBe(true);
    expect(owned.reader).toBe(true);
    for (const path of ['/api/session', `/api/end?id=${session}`, `/api/keepalive?id=${session}`]) {
      expect((await request.post(deviceURL + path, { data: Buffer.alloc(0) })).status(), path).toBe(409);
    }
    results = await page.evaluate(async (scans) => {
      const { bench } = globalThis;
      const results = [];
      for (const scan of scans) {
        const start = performance.now();
        try {
          await bench.reader.command(scan.command, scan.payload);
          throw new Error('Unexpected tag or missing no-tag status for ' + scan.family);
        } catch (error) {
          if (error.status === undefined) {
            throw error;
          }
          results.push({
            command: scan.command,
            status: error.status,
            data: [...error.data],
            elapsedMs: Math.round(performance.now() - start)
          });
        }
      }
      return results;
    }, scans);
    expect(results.map((result) => [result.command, result.status, ...result.data])).toEqual(
      baseline.map((result) => [result.command, result.status])
    );
    bleTearoff = await page.evaluate(
      async (body) => {
        const { bench } = globalThis;
        const path = bench.ST25TB.TEAROFF_PATH;
        const id = bench.reader.session;
        await bench.request(`${path}/start?id=${id}`, new Uint8Array(body));
        const state = () =>
          bench.request(`${path}/state?id=${id}`, new Uint8Array()).then(bench.decodeSt25tbTearoff);
        let current = await state();
        if (current.state !== 2 || current.attempts) {
          throw new Error('Expected a running tear-off job with zero writes');
        }
        let rejected;
        try {
          await bench.reader.command(2304);
        } catch (error) {
          rejected = error.httpStatus;
        }
        if (rejected !== 409) {
          throw new Error('A running tear-off job accepted a reader command');
        }
        const start = performance.now();
        await bench.request(`${path}/cancel?id=${id}`, new Uint8Array());
        while (current.state !== bench.ST25TB.DONE && performance.now() - start < 5000) {
          current = await state();
          if (current.state !== bench.ST25TB.DONE) {
            await new Promise((resolve) => setTimeout(resolve, 50));
          }
        }
        return { ...current, cancelElapsedMs: Math.round(performance.now() - start) };
      },
      [...tearoffBody]
    );
    for (const key of ['state', 'outcome', 'block', 'flags', 'current', 'target', 'attempts', 'delay_us']) {
      expect(bleTearoff[key], key).toBe(usbTearoff[key]);
    }
    expect(bleTearoff.cancelElapsedMs).toBeLessThan(5000);
    expect(leakedHttp).toEqual([]);
    checkChooser();
    const running = await page.evaluate(
      async (body) => {
        const { bench } = globalThis;
        const path = bench.ST25TB.TEAROFF_PATH;
        const id = bench.reader.session;
        await bench.request(`${path}/start?id=${id}`, new Uint8Array(body));
        return bench.decodeSt25tbTearoff(await bench.request(`${path}/state?id=${id}`, new Uint8Array()));
      },
      [...tearoffBody]
    );
    expect(running.state).toBe(2);
    expect(running.attempts).toBe(0);
    disconnectedDuringTearoff = true;
    // Deliberately omit CANCEL and END for this second job. Closing the real
    // GATT link must cancel it, release its lease and restore emulator mode.
    qualified = true;
  } finally {
    const disconnectedAt = Date.now();
    await page.close();
    await expect.poll(async () => (await info()).busy).toBe(false);
    const disconnectCleanupMs = Date.now() - disconnectedAt;
    let disconnectJobState;
    if (disconnectedDuringTearoff) {
      const opened = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
      expect(opened.status()).toBe(200);
      const verificationSession = (await opened.json()).session;
      try {
        const response = await request.post(
          `${deviceURL}${ST25TB.TEAROFF_PATH}/state?id=${verificationSession}`,
          { data: Buffer.alloc(0) }
        );
        expect(response.status()).toBe(200);
        disconnectJobState = decodeSt25tbTearoff(new Uint8Array(await response.body()));
        expect(disconnectJobState).toMatchObject({
          state: ST25TB.DONE,
          outcome: 1,
          flags: ST25TB.CLEANUP_COMPLETE,
          attempts: 0
        });
      } finally {
        expect(
          (
            await request.post(`${deviceURL}/api/end?id=${verificationSession}`, {
              data: Buffer.alloc(0)
            })
          ).status()
        ).toBe(200);
      }
    }
    const after = await info();
    for (const key of ['chip', 'reader', 'active_slot', 'settings']) {
      expect(after[key], key).toEqual(before[key]);
    }
    for (let slot = 0; slot < 8; slot++) {
      expect(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body()).toEqual(metadata[slot]);
    }
    await testInfo.attach('physical-reader-no-tag-evidence', {
      body: JSON.stringify(
        {
          qualified,
          chip: after.chip,
          firmware: after.git,
          positiveRfDecodeQualified: false,
          physicalWrites: false,
          baseline,
          bluetooth: results,
          tearoffWithoutTag: { usb: usbTearoff, bluetooth: bleTearoff },
          disconnectedDuringTearoff,
          disconnectCleanupMs,
          disconnectJobState,
          httpFallbackRequests: leakedHttp,
          readerLeaseReleasedOnDisconnect: true
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
