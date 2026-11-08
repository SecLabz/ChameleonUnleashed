import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { readFile, writeFile } from 'node:fs/promises';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';

const deviceURL = 'http://192.168.16.1';

test('compare real Classic nonce acquisition over USB and BLE without a PM3', async ({
  page,
  request
}, testInfo) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_CAPTURE_SPEED !== '1',
    'Requires an explicit private Classic fixture'
  );
  test.setTimeout(180000);
  const fixture = JSON.parse(await readFile(process.env.CHAMELEON_CLASSIC_CAPTURE_FIXTURE, 'utf8'));
  const before = await (await request.get(deviceURL + '/api/device')).json();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const slots = await usbSlotSnapshot(request, deviceURL);
  const chooser = await physicalChooser(page);
  const saveTrace = await traceGatt(page);
  const acknowledged = process.env.CHAMELEON_BLE_TEST_GATT_ACKED === '1';
  const bundle = await build({
    stdin: {
      resolveDir: process.cwd(),
      contents: `
    import { connectBluetooth } from './src/device/bluetooth.mjs';
    import { request, setDeviceTransport } from './src/device/transport.mjs';
    const bench = globalThis.bench = { request };
    document.querySelector('button').onclick = () => {
      bench.connecting = connectBluetooth().then(transport => {
        if (${acknowledged}) transport.writeWithoutResponse = false;
        bench.transport = transport; setDeviceTransport(transport);
        return request('/api/device');
      });
    };
  `
    },
    bundle: true,
    format: 'esm',
    write: false
  });
  await page.route('**/capture-speed', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<meta name="chameleon-hosted" content="1"><button>Connect capture Ultra</button><script type="module" src="./capture-speed.mjs"></script>'
    })
  );
  await page.route('**/capture-speed.mjs', (route) =>
    route.fulfill({ contentType: 'text/javascript', body: bundle.outputFiles[0].text })
  );
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  let lease = 0;
  let useBle = false;
  const samples = [];
  const raw = async (path, bytes) => {
    if (useBle) {
      return await page.evaluate(
        async ({ path, bytes }) => {
          const value = await globalThis.bench.request(path, Uint8Array.from(bytes));
          return value instanceof Uint8Array ? [...value] : value;
        },
        { path, bytes }
      );
    }
    const reply = await request.post(deviceURL + path, { data: Buffer.from(bytes), timeout: 15000 });
    expect(reply.status()).toBe(200);
    return reply.headers()['content-type']?.includes('json') ? await reply.json() : [...(await reply.body())];
  };
  const command = async (number, body) => {
    const reply = await raw(`/api/command/${number}?id=${lease}`, body);
    expect(reply.slice(0, 2)).toEqual([0, number === 2026 ? 0x68 : 0]);
    return reply.slice(2);
  };
  const identify = async () => {
    const reply = await command(2000, []);
    expect(reply[0]).toBe(fixture.card.uid.length);
    expect(reply.slice(1, 1 + reply[0])).toEqual(fixture.card.uid);
  };
  const phase = async (name, ble) => {
    useBle = ble;
    lease = (await raw('/api/session', [])).session;
    try {
      await identify();
      await command(2026, [1]);
      for (let round = 0; round < 12; round++) {
        const diagnostic = round < 2;
        const body = [
          fixture.source.is_key_b ? 0x61 : 0x60,
          fixture.source.sector * 4,
          ...fixture.source.key,
          fixture.target.is_key_b ? 0x61 : 0x60,
          fixture.target.sector * 4,
          0,
          16,
          ...(diagnostic ? [1] : [])
        ];
        const start = performance.now();
        const reply = await command(2023, body);
        const elapsedMs = performance.now() - start;
        const offset = diagnostic ? 40 : 0;
        expect(reply.length).toBe(offset + 16 * 12);
        const observations = [];
        for (let i = offset; i < reply.length; i += 12) {
          expect(reply.slice(i + 8, i + 12).every((value) => value === 0 || value === 1)).toBe(true);
          observations.push(reply.slice(i, i + 12));
        }
        let profile;
        if (diagnostic) {
          const view = new DataView(Uint8Array.from(reply).buffer);
          expect(view.getUint32(0)).toBe(1);
          profile = Array.from({ length: 9 }, (_, i) => view.getUint32(4 + i * 4));
          expect(profile[8]).toBe(16);
        }
        samples.push({ phase: name, round, elapsedMs, observations: 16, diagnostic, profile });
        await writeFile(testInfo.outputPath(`${name}-${round}.json`), JSON.stringify(observations));
        await writeFile(testInfo.outputPath('capture-progress.json'), JSON.stringify(samples, null, 2));
      }
      await identify();
    } finally {
      await raw(`/api/end?id=${lease}`, []);
      lease = 0;
    }
  };
  let trace;
  try {
    await phase('usb-before', false);
    await page.goto('./capture-speed');
    await page.getByRole('button', { name: 'Connect capture Ultra' }).click();
    expect((await page.evaluate(() => globalThis.bench.connecting)).chip).toBe(before.chip);
    await phase('usb-ble-idle', false);
    await phase('ble', true);
    await page.evaluate(() => globalThis.bench.transport.disconnect());
    await phase('usb-after', false);
    chooser();
    trace = await saveTrace(testInfo.outputPath('gatt-trace.json'));
    expect(trace.errors).toEqual([]);
    expect(trace.pending).toEqual([]);
    expect(leakedHttp).toEqual([]);
    expect(
      trace.requests.filter((x) => x.operation === 8 && ![2000, 2023, 2026].includes(x.argument))
    ).toEqual([]);
  } finally {
    await page.close();
    await expect
      .poll(async () => (await request.get(deviceURL + '/api/device')).json().then((x) => x.busy))
      .toBe(false);
    const after = await (await request.get(deviceURL + '/api/device')).json();
    for (const key of ['chip', 'git', 'reader', 'active_slot', 'settings']) {
      expect(after[key]).toEqual(before[key]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(slots);
    await writeFile(
      testInfo.outputPath('capture-speed.json'),
      JSON.stringify(
        {
          firmware: before.git,
          acknowledged,
          samples,
          gattOperations: trace?.requests.length,
          preserved: true,
          tagWrites: false,
          pm3Used: false
        },
        null,
        2
      )
    );
  }
});
