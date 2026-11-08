import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { createHash } from 'node:crypto';
import { physicalChooser } from './chooser.mjs';
import { slotMatrixFixtures } from './slot-matrix-fixtures.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';
import { decodeSlot } from '../../src/device/slots.mjs';
import { be32 } from '../../src/documents/slot-image.mjs';

const deviceURL = 'http://192.168.16.1';
const name = (type) => `BLE matrix ${type} é`;
const hash = (bytes) => createHash('sha256').update(Buffer.from(bytes)).digest('hex');

test('physical BLE round-trips all 25 slot formats and abandons a large staged upload atomically', async ({
  page,
  request
}, testInfo) => {
  test.skip(process.env.CHAMELEON_BLE_TEST_SLOT_MATRIX !== '1', 'Requires explicitly reserved empty slot 8');
  test.setTimeout(300000);
  page.setDefaultTimeout(15000);
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  expect(before.active_slot).not.toBe(7);
  const baseline = await usbSlotSnapshot(request, deviceURL);
  expect(baseline.metadata[7].slice(0, 7)).toEqual(Array(7).fill(0));
  const fixtures = await slotMatrixFixtures();
  expect(fixtures).toHaveLength(25);
  expect(new Set(fixtures.map((fixture) => fixture.type)).size).toBe(25);
  expect(fixtures.find((fixture) => fixture.type === 1300).bytes).toHaveLength(4497);

  const bundled = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
      import { connectBluetooth } from './src/device/bluetooth.mjs';
      import { request, setDeviceTransport } from './src/device/transport.mjs';
      import { SlotDevice, decodeSlot } from './src/device/slots.mjs';
      import { decodeSlotImage, encodeSlotImage, be32 } from './src/documents/slot-image.mjs';
      const bench = globalThis.bench = { request, SlotDevice, decodeSlot, decodeSlotImage, encodeSlotImage, be32 };
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
  await page.route('**/slot-matrix-bench', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<meta name="chameleon-hosted" content="1"><button>Connect matrix Ultra</button><script type="module" src="./slot-matrix-bench.mjs"></script>'
    })
  );
  await page.route('**/slot-matrix-bench.mjs', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: bundled.outputFiles[0].text
    })
  );
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  const records = [];
  const fallbackClears = [];
  const expected = new Map();
  let uploadChunks;
  let partial;
  let qualified = false;

  const verifyUsbTarget = async () => {
    const snapshot = await usbSlotSnapshot(request, deviceURL, [7]);
    const metadata = decodeSlot(new Uint8Array(snapshot.metadata[7]));
    expect(snapshot.images).toHaveLength(expected.size);
    for (const [sense, fixture] of expected) {
      const image = snapshot.images.find((image) => image.sense === sense);
      expect(image?.type).toBe(fixture.type);
      expect(image?.bytes).toEqual(fixture.bytes);
      const place = metadata[sense === 2 ? 'hf' : 'lf'];
      expect(place.name).toBe(name(fixture.type));
      expect(place.enabled).toBe(false);
    }
    return snapshot;
  };
  const save = async (fixture) => {
    const result = await page.evaluate(
      async ({ fixture, name }) => {
        const { bench } = globalThis;
        const slots = (bench.slots = new bench.SlotDevice());
        bench.uploadChunks ??= [];
        const command = slots.command.bind(slots);
        slots.command = async (operation, bytes = []) => {
          const result = await command(operation, bytes);
          if (operation === 'write') {
            bench.uploadChunks.push({
              type: fixture.type,
              offset: bytes[0] * 256 + bytes[1],
              bodyBytes: bytes.length,
              acknowledged: [...result]
            });
          }
          return result;
        };
        await slots.begin();
        try {
          const field = fixture.sense === 2 ? 'hf' : 'lf';
          const other = fixture.sense === 2 ? 'lf' : 'hf';
          const before = bench.decodeSlot(await bench.request('/api/slot/7'));
          const start = performance.now();
          const document = bench.decodeSlotImage(new Uint8Array(fixture.bytes), name);
          const saved = await slots.save(7, fixture.sense, document, name, before[field].revision, false);
          if (JSON.stringify([...bench.encodeSlotImage(saved)]) !== JSON.stringify(fixture.bytes)) {
            throw new Error('BLE save/readback differs from the fixture');
          }
          const after = bench.decodeSlot(await bench.request('/api/slot/7'));
          if (JSON.stringify(after[other]) !== JSON.stringify(before[other])) {
            throw new Error('Saving changed the other frequency metadata or revision');
          }
          return {
            elapsedMs: performance.now() - start,
            otherMetadataPreserved: true,
            revision: after[field].revision
          };
        } finally {
          await slots.end();
        }
      },
      { fixture, name: name(fixture.type) }
    );
    expected.set(fixture.sense, fixture);
    await verifyUsbTarget();
    records.push({
      type: fixture.type,
      sense: fixture.sense,
      bytes: fixture.bytes.length,
      sha256: hash(fixture.bytes),
      ...result,
      independentUsbReadback: true
    });
  };

  try {
    await page.goto('./slot-matrix-bench');
    await page.getByRole('button', { name: 'Connect matrix Ultra', exact: true }).click();
    expect((await page.evaluate(() => globalThis.bench.connecting)).chip).toBe(before.chip);
    // Keep a real LF sentinel throughout the HF formats, including large/small transitions.
    await save(fixtures.find((fixture) => fixture.type === 100));
    for (const fixture of fixtures.filter((fixture) => fixture.sense === 2)) {
      await save(fixture);
    }
    // The 4497-byte DESFire HF image remains intact throughout all eight LF formats.
    for (const fixture of fixtures.filter((fixture) => fixture.sense === 1)) {
      await save(fixture);
    }
    const persisted = await verifyUsbTarget();
    const classic = fixtures.find((fixture) => fixture.type === 1003);
    partial = await page.evaluate(async (fixture) => {
      const { bench } = globalThis;
      const slots = (bench.slots = new bench.SlotDevice());
      await slots.begin();
      const metadata = bench.decodeSlot(await bench.request('/api/slot/7'));
      await slots.command('begin', [
        7,
        2,
        fixture.bytes.length >> 8,
        fixture.bytes.length & 255,
        ...bench.be32(metadata.hf.revision)
      ]);
      for (const offset of [0, 1000]) {
        const reply = await slots.command('write', [
          offset >> 8,
          offset & 255,
          ...fixture.bytes.slice(offset, offset + 1000)
        ]);
        if (reply[0] * 256 + reply[1] !== offset + 1000) {
          throw new Error('Partial upload acknowledgement differs');
        }
      }
      const session = slots.session;
      clearInterval(slots.timer);
      slots.session = 0;
      bench.transport.disconnect();
      return { session, type: fixture.type, stagedBytes: 2000, totalBytes: fixture.bytes.length };
    }, classic);
    await expect.poll(async () => (await info()).busy).toBe(false);
    expect(await verifyUsbTarget()).toEqual(persisted);
    await page.getByRole('button', { name: 'Connect matrix Ultra', exact: true }).click();
    expect((await page.evaluate(() => globalThis.bench.connecting)).chip).toBe(before.chip);
    const staleStatus = await page.evaluate(async (session) => {
      try {
        await globalThis.bench.request(`/api/keepalive?id=${session}`, new Uint8Array());
      } catch (error) {
        return error.httpStatus;
      }
    }, partial.session);
    expect(staleStatus).toBe(409);
    partial = { ...partial, staleStatus, persistedImagesPreserved: true };
    uploadChunks = await page.evaluate(() => globalThis.bench.uploadChunks);
    expect(uploadChunks.some((chunk) => chunk.bodyBytes === 1002)).toBe(true);
    expect(uploadChunks.every((chunk) => chunk.bodyBytes <= 1002)).toBe(true);
    await page.evaluate(async () => {
      const { bench } = globalThis;
      const slots = (bench.slots = new bench.SlotDevice());
      await slots.begin();
      try {
        for (const [sense, field] of [
          [2, 'hf'],
          [1, 'lf']
        ]) {
          const current = bench.decodeSlot(await bench.request('/api/slot/7'))[field];
          await slots.action('clear', [7, sense, ...bench.be32(current.revision)]);
        }
      } finally {
        await slots.end();
      }
    });
    expect(new Set(records.map((record) => record.type)).size).toBe(25);
    expect(leakedHttp).toEqual([]);
    checkChooser();
    qualified = true;
  } finally {
    await page.close();
    await expect.poll(async () => (await info()).busy).toBe(false);
    const current = decodeSlot(new Uint8Array(await (await request.get(deviceURL + '/api/slot/7')).body()));
    if (current.hf.type || current.lf.type) {
      for (const field of ['hf', 'lf']) {
        if (current[field].type) {
          expect(
            current[field].name.startsWith('BLE matrix '),
            'Refuse to erase unrecognized slot data'
          ).toBe(true);
        }
      }
      const lease = await request.post(deviceURL + '/api/slots/session', { data: Buffer.alloc(0) });
      expect(lease.status()).toBe(200);
      const { session } = await lease.json();
      try {
        for (const [sense, field] of [
          [2, 'hf'],
          [1, 'lf']
        ]) {
          if (current[field].type) {
            const reply = await request.post(`${deviceURL}/api/slots/clear?id=${session}`, {
              data: Buffer.from([7, sense, ...be32(current[field].revision)])
            });
            expect(reply.status()).toBe(200);
            expect([...(await reply.body())]).toEqual([0, 0x68]);
            fallbackClears.push(sense);
          }
        }
      } finally {
        expect(
          (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
        ).toBe(200);
      }
    }
    const after = await info();
    for (const key of ['chip', 'git', 'reader', 'active_slot', 'settings']) {
      expect(after[key], key).toEqual(before[key]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(baseline);
    await testInfo.attach('physical-slot-format-matrix', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          emptyTestSlot: 8,
          uniqueTypes: new Set(records.map((record) => record.type)).size,
          records,
          uploadChunks,
          partial,
          fallbackClears,
          httpFallbackRequests: leakedHttp,
          initialImages: baseline.images.map(({ slot, sense, type, bytes }) => ({
            slot,
            sense,
            type,
            bytes: bytes.length,
            sha256: hash(bytes)
          })),
          pm3Used: false,
          physicalRfQualified: false,
          syntheticFixtures: true
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
