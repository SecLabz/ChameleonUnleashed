import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { readFile, writeFile } from 'node:fs/promises';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';

const deviceURL = 'http://192.168.16.1';

test('measure Classic link profiles, batched reads and autonomous jobs on the same physical card', async ({
  page,
  request
}, testInfo) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_CLASSIC_LINK_SPEED !== '1',
    'Requires an explicit placed Classic tag and private fixture with sector keys'
  );
  test.setTimeout(240000);
  const rounds = Number(process.env.CHAMELEON_BLE_TEST_CLASSIC_LINK_ROUNDS || 3);
  expect([1, 2, 3]).toContain(rounds);
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
    import { encodeOperation, OPERATION } from './src/device/ble-protocol.mjs';
    import { request, setDeviceTransport, classicJob } from './src/device/transport.mjs';
    const bench = globalThis.bench = { request, classicJob };
    document.querySelector('button').onclick = () => {
      bench.connecting = connectBluetooth().then(transport => {
        if (${acknowledged}) transport.writeWithoutResponse = false;
        bench.transport = transport;
        bench.profile = (profile=0, phy=0) => transport.run(c => encodeOperation(OPERATION.LINK_PROFILE,c,0,0,Uint8Array.from([profile,phy]))).then(reply=>[...reply.data]);
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
  const referenceSectors = [];
  const profiles = [];
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
  const runJob = async (body, stop = false) => {
    if (useBle) {
      return page.evaluate(
        async ({ lease, body, stop }) => {
          const controller = new AbortController();
          const events = [];
          let stoppedAt;
          const result = await globalThis.bench.classicJob(lease, body, {
            signal: controller.signal,
            onEvent: (event) => {
              events.push({ ...event, data: [...event.data] });
              if (stop && !event.done && !stoppedAt) {
                stoppedAt = performance.now();
                controller.abort();
              }
            }
          });
          return {
            events,
            result: { ...result, data: [...result.data] },
            cancelMs: stoppedAt ? performance.now() - stoppedAt : null
          };
        },
        { lease, body, stop }
      );
    }
    const start = await raw(`/api/classic/job/start?id=${lease}`, body);
    expect(start.length).toBe(4);
    const id = Buffer.from(start);
    const events = [];
    let stoppedAt;
    for (;;) {
      const data = await raw(`/api/classic/job/read?id=${lease}`, [...id]);
      if (!data.length) {
        await new Promise((resolve) => setTimeout(resolve, 5));
        continue;
      }
      const view = new DataView(Uint8Array.from(data).buffer);
      expect(view.getUint32(1)).toBe(id.readUInt32BE());
      expect(view.getUint16(5)).toBe(events.length);
      const event = {
        done: !!(data[7] & 1),
        cancelled: !!(data[7] & 2),
        status: view.getUint16(9),
        completed: view.getUint16(11),
        data: data.slice(13)
      };
      events.push(event);
      if (stop && !event.done && !stoppedAt) {
        stoppedAt = performance.now();
        await raw(`/api/classic/job/cancel?id=${lease}`, [...id]);
      }
      if (event.done) {
        return { events, result: event, cancelMs: stoppedAt ? performance.now() - stoppedAt : null };
      }
    }
  };
  const phase = async (name, ble) => {
    useBle = ble;
    lease = (await raw('/api/session', [])).session;
    try {
      await identify();
      for (let round = 0; round < 12; round++) {
        const start = performance.now();
        await raw(`/api/keepalive?id=${lease}`, []);
        samples.push({ phase: name, kind: 'keepalive', round, elapsedMs: performance.now() - start });
      }
      for (let sector = 0; sector < 16; sector++) {
        const start = performance.now();
        const reply = await command(2027, [
          0x60,
          sector,
          ...fixture.keys[sector],
          fixture.card.uid.length,
          ...fixture.card.uid
        ]);
        expect(reply.length).toBe(66);
        expect(reply.slice(0, 2)).toEqual([0, 15]);
        if (name === 'usb-before') {
          referenceSectors[sector] = reply;
        } else {
          expect(reply).toEqual(referenceSectors[sector]);
        }
        samples.push({ phase: name, kind: 'sector', sector, elapsedMs: performance.now() - start });
      }
      await command(2026, [1]);
      const badKey = [...fixture.source.key];
      badKey[0] ^= 1;
      for (let round = 0; round < 20; round++) {
        const start = performance.now();
        const reply = await raw(`/api/command/2015?id=${lease}`, [0, 0x60, 1, ...badKey]);
        expect(reply.slice(0, 2)).toEqual([0, 6]);
        samples.push({ phase: name, kind: 'key-reject', round, elapsedMs: performance.now() - start });
      }
      for (let round = 0; round < rounds; round++) {
        const start = performance.now();
        for (let first = 0; first < 16; first += 6) {
          const sectors = Array.from({ length: Math.min(6, 16 - first) }, (_, i) => first + i);
          const reply = await command(2042, [
            1,
            1,
            fixture.card.uid.length,
            ...fixture.card.uid,
            sectors.length,
            ...sectors.flatMap((sector) => [sector, 0x60, ...fixture.keys[sector]])
          ]);
          expect(reply.slice(0, 3)).toEqual([1, 1, sectors.length]);
          let offset = 3;
          for (const sector of sectors) {
            expect(reply.slice(offset, offset + 4)).toEqual([0, 0, 0, 66]);
            offset += 4;
            expect(reply.slice(offset, offset + 66)).toEqual(referenceSectors[sector]);
            offset += 66;
          }
          expect(offset).toBe(reply.length);
        }
        samples.push({ phase: name, kind: 'batch-dump16', round, elapsedMs: performance.now() - start });
      }
      {
        const start = performance.now();
        for (let first = 0; first < 20; first += 8) {
          const count = Math.min(8, 20 - first);
          const reply = await command(2042, [
            1,
            0,
            fixture.card.uid.length,
            ...fixture.card.uid,
            count,
            ...Array.from({ length: count }, () => [0, 0x60, ...badKey]).flat()
          ]);
          expect(reply.slice(0, 3)).toEqual([1, 0, count]);
          for (let i = 0; i < count; i++) {
            expect(reply.slice(3 + i * 11, 7 + i * 11)).toEqual([0, 6, 0, 7]);
          }
        }
        samples.push({ phase: name, kind: 'batch-reject20', elapsedMs: performance.now() - start });
      }
      const prefix = [1, 1, fixture.card.uid.length, ...fixture.card.uid];
      for (let round = 0; round < rounds; round++) {
        const start = performance.now();
        const job = await runJob([
          ...prefix,
          16,
          ...fixture.keys.flatMap((key, sector) => [sector, 0x60, ...key])
        ]);
        expect(job.events.length).toBe(16);
        expect(job.result.completed).toBe(16);
        job.events.forEach((event, sector) => {
          expect(event.status).toBe(0);
          expect(event.data).toEqual(referenceSectors[sector]);
        });
        samples.push({ phase: name, kind: 'job-dump16', round, elapsedMs: performance.now() - start });
      }
      {
        const start = performance.now();
        const job = await runJob([
          1,
          0,
          fixture.card.uid.length,
          ...fixture.card.uid,
          20,
          ...Array.from({ length: 20 }, () => [0, 0x60, ...badKey]).flat()
        ]);
        expect(job.events.length).toBe(20);
        expect(job.events.every((event) => event.status === 6)).toBe(true);
        samples.push({ phase: name, kind: 'job-reject20', elapsedMs: performance.now() - start });
      }
      const capturePrefix = [
        1,
        2,
        fixture.card.uid.length,
        ...fixture.card.uid,
        0x60,
        0,
        ...fixture.source.key,
        0x60,
        4,
        0,
        16
      ];
      for (let round = 0; round < rounds; round++) {
        const start = performance.now();
        const job = await runJob([...capturePrefix, 2, 0]);
        expect(job.result.completed).toBe(512);
        expect(job.events.length).toBe(32);
        for (const event of job.events) {
          expect(event.status).toBe(0);
          expect(event.data.length).toBe(192);
          for (let i = 8; i < 192; i += 12) {
            expect(event.data.slice(i, i + 4).every((x) => x === 0 || x === 1)).toBe(true);
          }
        }
        samples.push({ phase: name, kind: 'job-capture512', round, elapsedMs: performance.now() - start });
      }
      {
        const job = await runJob([...capturePrefix, 16, 0], true);
        expect(job.result.cancelled).toBe(true);
        expect(job.result.completed).toBeGreaterThanOrEqual(16);
        expect(job.result.completed).toBeLessThan(128);
        expect(job.cancelMs).toBeLessThan(500);
        samples.push({
          phase: name,
          kind: 'job-stop',
          elapsedMs: job.cancelMs,
          retained: job.result.completed
        });
        const wrong = [...capturePrefix, 0, 16];
        wrong[3] ^= 1;
        const absent = await runJob(wrong);
        expect(absent.result.status).toBe(1);
        expect(absent.result.completed).toBe(0);
        const partial = await runJob([...prefix, 2, 0, 0x60, ...badKey, 1, 0x60, ...fixture.keys[1]]);
        expect(partial.events.map((x) => x.status)).toEqual([6, 0]);
        expect(partial.events[1].data).toEqual(referenceSectors[1]);
      }
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
    for (const [profile, phy] of [
      [4, 0],
      [2, 0]
    ]) {
      const response = await page.evaluate(
        async ({ profile, phy }) => await globalThis.bench.profile(profile, phy),
        { profile, phy }
      );
      expect(response.length).toBe(24);
      await page.waitForTimeout(2000);
      const actual = await page.evaluate(() => globalThis.bench.profile());
      const name = `ble-${(actual[4] * 256 + actual[5]) * 1.25}ms`;
      profiles.push({ phase: name, requested: { profile, phy }, response, actual });
      await phase(name, true);
      profiles.at(-1).after = await page.evaluate(() => globalThis.bench.profile());
    }
    await page.evaluate(() => globalThis.bench.transport.disconnect());
    await phase('usb-after', false);
    chooser();
    trace = await saveTrace(testInfo.outputPath('gatt-trace.json'));
    expect(trace.errors).toEqual([]);
    expect(trace.pending).toEqual([]);
    expect(leakedHttp).toEqual([]);
    expect(
      trace.requests.filter(
        (x) => x.operation === 8 && ![2000, 2015, 2023, 2026, 2027, 2042].includes(x.argument)
      )
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
          profiles,
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
