import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { createHash } from 'node:crypto';
import { physicalChooser } from './chooser.mjs';

const deviceURL = 'http://192.168.16.1';
const stableDevice = ({ chip, git, reader, active_slot, settings }) => ({
  chip,
  git,
  reader,
  active_slot,
  settings
});
const hash = (bytes) => createHash('sha256').update(Buffer.from(bytes)).digest('hex');
const summary = (values) => {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return {
    count: sorted.length,
    minMs: sorted[0],
    medianMs: sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2,
    maxMs: sorted.at(-1)
  };
};

// A descriptive benchmark: it asserts data fidelity and cleanup, not an arbitrary speed SLA.
test('the physical USB and BLE transports preserve data during measured reads and queued requests', async ({
  page,
  context,
  request
}, testInfo) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_PERFORMANCE !== '1',
    'Requires explicit physical benchmark opt-in'
  );
  test.setTimeout(180000);
  page.setDefaultTimeout(15000);
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);

  async function snapshot() {
    const metadata = [];
    for (let slot = 0; slot < 8; slot++) {
      const reply = await request.get(`${deviceURL}/api/slot/${slot}`);
      expect(reply.status()).toBe(200);
      metadata.push([...(await reply.body())]);
    }
    const lease = await request.post(deviceURL + '/api/slots/session', { data: Buffer.alloc(0) });
    expect(lease.status()).toBe(200);
    const { session } = await lease.json();
    const images = [];
    try {
      for (let slot = 0; slot < 8; slot++) {
        for (const [sense, offset] of [
          [2, 0],
          [1, 2]
        ]) {
          const type = metadata[slot][offset] * 256 + metadata[slot][offset + 1];
          if (!type) {
            continue;
          }
          const reply = await request.post(`${deviceURL}/api/slots/open?id=${session}`, {
            data: Buffer.from([slot, sense])
          });
          expect(reply.status()).toBe(200);
          const opened = [...(await reply.body())];
          expect(opened.length).toBe(6);
          const size = opened[0] * 256 + opened[1];
          expect(size).toBeGreaterThan(0);
          const bytes = [];
          for (let start = 0; start < size; start += 512) {
            const count = Math.min(512, size - start);
            const part = await request.post(`${deviceURL}/api/slots/read?id=${session}`, {
              data: Buffer.from([start >> 8, start & 255, count >> 8, count & 255])
            });
            expect(part.status()).toBe(200);
            const body = await part.body();
            expect(body.length).toBe(count);
            bytes.push(...body);
          }
          images.push({ slot, sense, type, opened, bytes });
        }
      }
    } finally {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    return { metadata, images };
  }

  const baseline = await snapshot();
  expect(baseline.images.some((image) => image.bytes.length >= 1024)).toBe(true);
  const bundled = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
        import { connectBluetooth } from './src/device/bluetooth.mjs';
        import { request, setDeviceTransport } from './src/device/transport.mjs';
        import { SlotDevice } from './src/device/slots.mjs';
        const bench = globalThis.bench = { request, SlotDevice };
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
  const usbPage = await context.newPage();
  const install = async (target, hosted) => {
    await target.route('**/performance-bench', (route) =>
      route.fulfill({
        contentType: 'text/html',
        body: `${hosted ? '<meta name="chameleon-hosted" content="1">' : ''}<button>Connect benchmark Ultra</button><script type="module" src="./performance-bench.mjs"></script>`
      })
    );
    await target.route('**/performance-bench.mjs', (route) =>
      route.fulfill({ contentType: 'text/javascript', body: bundled.outputFiles[0].text })
    );
  };
  // The real network document preserves Chromium's local address-space classification.
  // Only its module is replaced by the same transport harness used on the BLE page.
  await usbPage.route(deviceURL + '/app.mjs', (route) =>
    route.fulfill({ contentType: 'text/javascript', body: bundled.outputFiles[0].text })
  );
  await install(page, true);
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });

  const measure = async (target) =>
    await target.evaluate(
      async ({ baseline, expectedDevice }) => {
        const { bench } = globalThis;
        const same = (actual, expected, label) => {
          if (JSON.stringify(actual) !== JSON.stringify(expected)) {
            throw new Error(`${label}: data differs from independent USB snapshot`);
          }
        };
        const stable = ({ chip, git, reader, active_slot, settings }) => ({
          chip,
          git,
          reader,
          active_slot,
          settings
        });
        const timings = { device: [], queued: [], metadata: [], reads: [], images: [] };
        const started = performance.now();
        const timed = async (label, action, detail = {}) => {
          const start = performance.now();
          const value = await action();
          timings[label].push({ elapsedMs: performance.now() - start, ...detail });
          return value;
        };
        // Warm up outside the samples, then measure sequential and queued requests.
        same(stable(await bench.request('/api/device')), expectedDevice, 'warmup device');
        for (let round = 0; round < 12; round++) {
          const device = await timed('device', () => bench.request('/api/device'));
          same(stable(device), expectedDevice, 'device');
          if (device.busy) {
            throw new Error('Unexpected reservation during device reads');
          }
        }
        for (let round = 0; round < 3; round++) {
          await Promise.all(
            Array.from({ length: 8 }, async (_, index) => {
              const device = await timed('queued', () => bench.request('/api/device'), { round, index });
              same(stable(device), expectedDevice, 'queued device');
            })
          );
        }
        for (let round = 0; round < 3; round++) {
          for (let slot = 0; slot < 8; slot++) {
            const meta = await timed('metadata', () => bench.request(`/api/slot/${slot}`), { slot });
            same([...meta], baseline.metadata[slot], 'slot metadata');
          }
        }
        bench.slots = new bench.SlotDevice();
        await bench.slots.begin();
        try {
          for (let round = 0; round < 6; round++) {
            for (const image of baseline.images) {
              const start = performance.now();
              const opened = await bench.slots.command('open', [image.slot, image.sense]);
              same([...opened], image.opened, 'slot open and revision');
              const bytes = [];
              for (let offset = 0; offset < image.bytes.length; offset += 512) {
                const count = Math.min(512, image.bytes.length - offset);
                const part = await timed(
                  'reads',
                  () => bench.slots.command('read', [offset >> 8, offset & 255, count >> 8, count & 255]),
                  { bytes: count, round, slot: image.slot, sense: image.sense }
                );
                same([...part], image.bytes.slice(offset, offset + count), 'slot chunk');
                bytes.push(...part);
              }
              same(bytes, image.bytes, 'complete slot image');
              timings.images.push({
                elapsedMs: performance.now() - start,
                bytes: bytes.length,
                round,
                slot: image.slot,
                sense: image.sense
              });
            }
          }
        } finally {
          await bench.slots.end();
        }
        return { timings, elapsedMs: performance.now() - started, allDataMatched: true };
      },
      { baseline, expectedDevice: stableDevice(before) }
    );

  let usb;
  let ble;
  let connectMs;
  let qualified = false;
  try {
    await usbPage.goto(deviceURL + '/');
    await usbPage.waitForFunction(() => !!globalThis.bench);
    usb = await measure(usbPage);
    await usbPage.close();
    await page.goto('./performance-bench');
    const connectedAt = Date.now();
    await page.getByRole('button', { name: 'Connect benchmark Ultra', exact: true }).click();
    const connected = await page.evaluate(() => globalThis.bench.connecting);
    connectMs = Date.now() - connectedAt;
    expect(stableDevice(connected)).toEqual(stableDevice(before));
    ble = await measure(page);
    expect(leakedHttp).toEqual([]);
    checkChooser();
    qualified = true;
  } finally {
    await usbPage.close();
    await page.close();
    await expect.poll(async () => (await info()).busy).toBe(false);
    const after = await info();
    expect(stableDevice(after)).toEqual(stableDevice(before));
    expect(await snapshot()).toEqual(baseline);
    const summarize = (result) => {
      if (!result) {
        return null;
      }
      const { timings } = result;
      const fullChunks = timings.reads.filter((sample) => sample.bytes === 512);
      return {
        device: summary(timings.device.map((sample) => sample.elapsedMs)),
        queued: summary(timings.queued.map((sample) => sample.elapsedMs)),
        metadata: summary(timings.metadata.map((sample) => sample.elapsedMs)),
        full512ByteRead: summary(fullChunks.map((sample) => sample.elapsedMs)),
        full512ByteReadPayloadBytesPerSecond:
          (512 * fullChunks.length * 1000) /
          fullChunks.reduce((total, sample) => total + sample.elapsedMs, 0),
        images: baseline.images.map((image) => ({
          slot: image.slot,
          sense: image.sense,
          type: image.type,
          bytes: image.bytes.length,
          ...summary(
            timings.images
              .filter((sample) => sample.slot === image.slot && sample.sense === image.sense)
              .map((sample) => sample.elapsedMs)
          )
        })),
        totalMs: result.elapsedMs,
        allDataMatched: result.allDataMatched,
        raw: timings
      };
    };
    await testInfo.attach('physical-transport-performance', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          browser: 'Chromium on macOS; both transports use native browser requests and the app modules',
          usb: summarize(usb),
          ble: summarize(ble),
          bleConnectMsIncludingAutomatedChooser: connectMs,
          fixtureImages: baseline.images.map(({ slot, sense, type, bytes }) => ({
            slot,
            sense,
            type,
            bytes: bytes.length,
            sha256: hash(bytes)
          })),
          httpFallbackRequests: leakedHttp,
          readOnly: true,
          rfPerformanceQualified: false,
          batteryOperationQualified: false,
          pm3Used: false
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
