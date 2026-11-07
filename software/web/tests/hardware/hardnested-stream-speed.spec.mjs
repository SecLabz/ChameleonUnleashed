import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { readFile, writeFile } from 'node:fs/promises';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';

const deviceURL = 'http://192.168.16.1';

test('compare USB, legacy BLE and autonomous BLE acquisition with identical real workloads', async ({
  page,
  context,
  request
}, testInfo) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_NONCE_STREAM !== '1',
    'Requires a placed Classic card and private fixture'
  );
  const rounds = Number(process.env.CHAMELEON_BLE_TEST_NONCE_STREAM_ROUNDS || 3);
  expect(Number.isInteger(rounds) && rounds >= 1 && rounds <= 20).toBe(true);
  test.setTimeout(rounds * 45000 + 60000);
  const fixture = JSON.parse(await readFile(process.env.CHAMELEON_CLASSIC_CAPTURE_FIXTURE, 'utf8'));
  const info = async () => (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const slots = await usbSlotSnapshot(request, deviceURL);
  const chooser = await physicalChooser(page);
  const saveTrace = await traceGatt(page);
  const bundle = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
    import { connectBluetooth } from './src/device/bluetooth.mjs';
    import { setDeviceTransport } from './src/device/transport.mjs';
    import { Device } from './src/device/device.mjs';
    globalThis.bench = { Device, setDeviceTransport };
    document.querySelector('button').onclick = () => {
      bench.connecting = connectBluetooth().then(transport => { setDeviceTransport(transport); });
    };
  `
    },
    bundle: true,
    format: 'esm',
    write: false
  });
  await context.route('**/nonce-bench', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<meta name="chameleon-hosted" content="1"><button>Connect Ultra</button><script type="module" src="./nonce-bench.mjs"></script>'
    })
  );
  await context.route('**/nonce-bench.mjs', (route) =>
    route.fulfill({ contentType: 'text/javascript', body: bundle.outputFiles[0].text })
  );
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  const usbPage = await context.newPage();
  const samples = [];
  try {
    // A synthetic HTTP document loses the browser's local address-space status.
    // Load the real device page before adding the same measurement adapter.
    await usbPage.goto(deviceURL + '/#device');
    await usbPage.addScriptTag({ type: 'module', url: deviceURL + '/nonce-bench.mjs' });
    await usbPage.evaluate(() => globalThis.bench.setDeviceTransport(null));
    await page.goto('./nonce-bench');
    await page.bringToFront();
    await page.getByRole('button', { name: 'Connect Ultra', exact: true }).click();
    await page.evaluate(() => globalThis.bench.connecting);
    const orders = [
      ['usb', 'ble-legacy', 'ble-stream'],
      ['ble-stream', 'ble-legacy', 'usb'],
      ['ble-legacy', 'usb', 'ble-stream'],
      ['ble-stream', 'usb', 'ble-legacy'],
      ['ble-legacy', 'ble-stream', 'usb'],
      ['usb', 'ble-stream', 'ble-legacy']
    ];
    for (let round = 0; round < rounds; round++) {
      for (const mode of orders[round % orders.length]) {
        const sample = await (mode === 'usb' ? usbPage : page).evaluate(
          async ({ fixture, streaming }) => {
            const source = { sector: 0, is_key_b: false, key: fixture.source.key };
            const target = { sector: 1, is_key_b: false };
            const geometry = { sectors: [{ first: 0 }, { first: 4 }] };
            const device = new globalThis.bench.Device();
            let stream;
            try {
              const card = await device.begin();
              if (JSON.stringify(card.uid) !== JSON.stringify(fixture.card.uid)) {
                throw new Error('Wrong card');
              }
              const start = performance.now();
              let records = [];
              if (streaming) {
                stream = device.startNonceStream(source, target, 512, geometry);
                if (!stream) {
                  throw new Error('Native acquisition unavailable');
                }
                while (records.length < 512) {
                  const batch = await stream.read();
                  if (!batch.length) {
                    throw new Error('Incomplete acquisition');
                  }
                  records.push(...batch);
                }
                await stream.stop();
              } else {
                await device.command(2026, [1]);
                for (let i = 0; i < 512; i += 16) {
                  records.push(...(await device.collectNonces(source, target, 16, geometry)));
                }
                await device.command(2026, [0]);
              }
              return {
                streaming,
                elapsedMs: performance.now() - start,
                count: records.length,
                valid: records.every(
                  (r) => r.parity.length === 4 && r.parity.every((b) => b === 0 || b === 1)
                )
              };
            } finally {
              try {
                if (stream) {
                  await stream.stop();
                }
              } finally {
                await device.end();
              }
            }
          },
          { fixture, streaming: mode === 'ble-stream' }
        );
        samples.push({ mode, round, ...sample });
        await writeFile(testInfo.outputPath('samples.json'), JSON.stringify(samples, null, 2));
        console.log(`${mode} ${round + 1}/${rounds}: ${sample.elapsedMs.toFixed(1)} ms`);
      }
    }
    expect(samples).toHaveLength(rounds * 3);
    expect(samples.every((s) => s.count === 512 && s.valid)).toBe(true);
    await writeFile(testInfo.outputPath('samples.json'), JSON.stringify(samples, null, 2));
    const trace = await saveTrace(testInfo.outputPath('gatt-trace.json'));
    expect(trace.errors).toEqual([]);
    expect(trace.pending).toEqual([]);
    expect(trace.truncated).toBe(false);
    expect(trace.requests.filter((r) => r.operation === 25)).toHaveLength(rounds);
    expect(trace.streamEvents).toHaveLength(rounds * 32);
    expect(leakedHttp).toEqual([]);
    chooser();
  } finally {
    await writeFile(testInfo.outputPath('samples.json'), JSON.stringify(samples, null, 2));
    await usbPage.close();
    await page.close();
    await expect.poll(async () => (await info()).busy).toBe(false);
    const after = await info();
    for (const field of ['chip', 'git', 'reader', 'active_slot', 'settings']) {
      expect(after[field]).toEqual(before[field]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(slots);
  }
});
