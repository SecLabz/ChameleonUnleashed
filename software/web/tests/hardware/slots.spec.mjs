import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { physicalChooser } from './chooser.mjs';

// Explicit opt-in in addition to the hardware config: this test writes ONLY
// empty slot 8, then clears it. It never writes to a physical RF tag.
test('physical BLE transactions preserve the other frequency and reject USB owners', async ({
  page,
  request
}, testInfo) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_EMPTY_SLOT_8 !== '1',
    'Requires an explicitly reserved empty slot 8'
  );
  test.setTimeout(180000);
  const before = await (await request.get('/api/device')).json();
  expect(before.busy).toBe(false);
  expect(before.active_slot).not.toBe(7);
  const baseline = [];
  for (let index = 0; index < 8; index++) {
    baseline.push([...(await (await request.get(`/api/slot/${index}`)).body())]);
  }
  expect(baseline[7].slice(0, 7)).toEqual([0, 0, 0, 0, 0, 0, 0]);
  for (let source = 0; source < 3; source++) {
    expect(baseline[source][0] * 256 + baseline[source][1]).toBe([1001, 1500, 2000][source]);
  }

  const bundled = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
        import { connectBluetooth } from './src/device/bluetooth.mjs';
        import { request, setDeviceTransport } from './src/device/transport.mjs';
        import { SlotDevice, decodeSlot } from './src/device/slots.mjs';
        import { encodeSlotImage, be32 } from './src/documents/slot-image.mjs';
        const bench = globalThis.bench = { request, SlotDevice, decodeSlot, encodeSlotImage, be32 };
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
  await page.route('**/hardware-bench', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<button>Connect test Ultra</button><script type="module" src="./hardware-bench.mjs"></script>'
    })
  );
  await page.route('**/hardware-bench.mjs', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: bundled.outputFiles[0].text
    })
  );
  const checkChooser = await physicalChooser(page);
  await page.goto('/hardware-bench');
  await page.getByRole('button', { name: 'Connect test Ultra' }).click();
  const info = await page.evaluate(() => globalThis.bench.connecting);
  expect(info.chip).toBe(before.chip);
  const evidence = [];
  try {
    const session = await page.evaluate(async () => {
      const { bench } = globalThis;
      bench.slots = new bench.SlotDevice();
      await bench.slots.begin();
      return bench.slots.session;
    });
    for (const path of ['/api/slots/session', `/api/end?id=${session}`, `/api/keepalive?id=${session}`]) {
      const denied = await request.post(path, { data: Buffer.alloc(0) });
      expect(denied.status(), path).toBe(409);
    }
    // Even after HTTP TCP connections close, the BLE-owned reservation lives.
    await page.evaluate(
      (id) => globalThis.bench.request(`/api/keepalive?id=${id}`, new Uint8Array()),
      session
    );

    const results = await page.evaluate(async () => {
      const { slots, request, decodeSlot, be32, encodeSlotImage } = globalThis.bench;
      const meta = async () => decodeSlot(await request('/api/slot/7'));
      const target = (sense, revision) => [7, sense, ...be32(revision)];
      const lf = { type: 100, kind: 'lf', unitBytes: 5, card: {}, memory: [[1, 2, 3, 4, 5]] };
      await slots.save(7, 1, lf, 'BLE test LF', (await meta()).lf.revision, false);
      const lfWire = [...encodeSlotImage(await slots.load(7, 1))];
      const results = [];
      for (let source = 0; source < 3; source++) {
        const sourceMeta = decodeSlot(await request(`/api/slot/${source}`));
        if (![1001, 1500, 2000].includes(sourceMeta.hf.type)) {
          throw new Error('This fixture requires Classic 1K, SLIX-L and ST25TB in slots 1–3');
        }
        const document = await slots.load(source, 2);
        const emptyRevision = (await meta()).hf.revision;
        const start = performance.now();
        await slots.save(7, 2, document, 'BLE test HF', emptyRevision, false);
        let current = await meta();
        if (current.lf.type !== 100 || current.lf.enabled || current.lf.name !== 'BLE test LF') {
          throw new Error('HF save changed LF metadata');
        }
        if (JSON.stringify([...encodeSlotImage(await slots.load(7, 1))]) !== JSON.stringify(lfWire)) {
          throw new Error('HF save changed LF image');
        }
        let staleStatus;
        try {
          await slots.action('clear', target(2, emptyRevision));
        } catch (error) {
          staleStatus = error.httpStatus;
        }
        if (staleStatus !== 409) {
          throw new Error('A stale slot revision was accepted');
        }
        await slots.action('nick', [
          ...target(2, current.hf.revision),
          ...new TextEncoder().encode('BLE vérifié')
        ]);
        current = await meta();
        if (current.hf.name !== 'BLE vérifié') {
          throw new Error('UTF-8 nickname was not preserved');
        }
        await slots.action('enable', [...target(2, current.hf.revision), 1]);
        current = await meta();
        if (!current.hf.enabled) {
          throw new Error('HF enable was not persisted');
        }
        await slots.action('clear', target(2, current.hf.revision));
        results.push({ type: document.type, elapsedMs: Math.round(performance.now() - start), staleStatus });
      }
      await slots.action('clear', target(1, (await meta()).lf.revision));
      return results;
    });
    evidence.push(...results);
    await page.evaluate(() => globalThis.bench.slots.end());

    // Abandon a partial upload. A physical disconnect must discard staging and
    // release the BLE lease, while the already-persisted empty slot stays empty.
    const staleSession = await page.evaluate(async () => {
      const { bench } = globalThis;
      await bench.slots.begin();
      const meta = bench.decodeSlot(await bench.request('/api/slot/7'));
      await bench.slots.command('begin', [7, 2, 0, 82, ...bench.be32(meta.hf.revision)]);
      await bench.slots.command('write', [0, 0, 2]);
      const session = bench.slots.session;
      clearInterval(bench.slots.timer);
      bench.slots.session = 0;
      bench.transport.disconnect();
      return session;
    });
    await expect.poll(async () => (await (await request.get('/api/device')).json()).busy).toBe(false);
    await page.getByRole('button', { name: 'Connect test Ultra' }).click();
    await page.evaluate(() => globalThis.bench.connecting);
    const staleStatus = await page.evaluate(async (id) => {
      const { bench } = globalThis;
      try {
        await bench.request(`/api/keepalive?id=${id}`, new Uint8Array());
      } catch (error) {
        return error.httpStatus;
      }
    }, staleSession);
    expect(staleStatus).toBe(409);
    checkChooser();
  } finally {
    await page.evaluate(() => {
      const { bench } = globalThis;
      clearInterval(bench.slots?.timer);
      bench.transport.disconnect();
    });
    await expect.poll(async () => (await (await request.get('/api/device')).json()).busy).toBe(false);
    // On failure the independent USB path can still clear the reserved test
    // slot. A normal run has already restored it entirely through BLE.
    const meta = await (await request.get('/api/slot/7')).body();
    if (meta.readUInt16BE(0) || meta.readUInt16BE(2)) {
      const { session } = await (await request.post('/api/slots/session', { data: Buffer.alloc(0) })).json();
      try {
        for (const [sense, offset, revisionOffset] of [
          [2, 0, meta.length - 8],
          [1, 2, meta.length - 4]
        ]) {
          if (meta.readUInt16BE(offset)) {
            const bytes = Buffer.alloc(6);
            bytes[0] = 7;
            bytes[1] = sense;
            bytes.writeUInt32BE(meta.readUInt32BE(revisionOffset), 2);
            expect((await request.post(`/api/slots/clear?id=${session}`, { data: bytes })).status()).toBe(
              200
            );
          }
        }
      } finally {
        await request.post(`/api/end?id=${session}`, { data: Buffer.alloc(0) });
      }
    }
  }
  const after = await (await request.get('/api/device')).json();
  for (const key of ['chip', 'reader', 'active_slot', 'settings']) {
    expect(after[key], key).toEqual(before[key]);
  }
  for (let index = 0; index < 8; index++) {
    expect([...(await (await request.get(`/api/slot/${index}`)).body())], `slot ${index + 1}`).toEqual(
      baseline[index]
    );
  }
  await testInfo.attach('physical-slot-transactions', {
    body: JSON.stringify({ chip: info.chip, git: info.git, emptyTestSlot: 8, results: evidence }, null, 2),
    contentType: 'application/json'
  });
});
