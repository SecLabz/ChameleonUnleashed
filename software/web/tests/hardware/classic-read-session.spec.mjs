import { test, expect } from '@playwright/test';
import { build } from 'esbuild';
import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';
// Private fixture: uid, 16 keys_a, and 64 raw_memory blocks from authenticated reads.
// Keep the masked Key A bytes returned by RF; an exported dump may replace them.
const web = fileURLToPath(new URL('../../', import.meta.url));
const url = 'http://192.168.16.1';
test('physical USB/BLE reads, UID guards, capture after rejection and Stop', async ({
  page,
  context,
  request
}, ti) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_CLASSIC_READ_SESSION !== '1',
    'Requires a dedicated Classic 1K and a private read fixture'
  );
  test.setTimeout(195000);
  const before = await (await request.get(url + '/api/device')).json();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const backup = JSON.parse(await readFile(process.env.CHAMELEON_CLASSIC_READ_SESSION_FIXTURE, 'utf8'));
  expect([4, 7].includes(backup.uid?.length)).toBe(true);
  expect(backup.keys_a?.length).toBe(16);
  expect(backup.raw_memory?.length).toBe(64);
  expect(
    backup.keys_a.every(
      (key) => key.length === 6 && key.every((b) => Number.isInteger(b) && b >= 0 && b <= 255)
    )
  ).toBe(true);
  const slots = await usbSlotSnapshot(request, url);
  const choose = await physicalChooser(page);
  const save = await traceGatt(page);
  const bundle = await build({
    stdin: {
      resolveDir: web,
      contents: `
 import {Device} from './src/device/device.mjs';
 import {setDeviceTransport,classicJob} from './src/device/transport.mjs';
 import {connectBluetooth} from './src/device/bluetooth.mjs';
 globalThis.rfBench={Device,setDeviceTransport,classicJob};
 document.querySelector('button').onclick=()=>{globalThis.rfBench.connecting=connectBluetooth().then(t=>{setDeviceTransport(t);globalThis.rfBench.transport=t;});};
 `
    },
    bundle: true,
    format: 'esm',
    write: false
  });
  await context.route('**/rf-bench', (r) =>
    r.fulfill({
      contentType: 'text/html',
      body: '<meta name="chameleon-hosted" content="1"><button>Connect RF Ultra</button><script type="module" src="./rf-bench.mjs"></script>'
    })
  );
  await context.route('**/rf-bench.mjs', (r) =>
    r.fulfill({ contentType: 'text/javascript', body: bundle.outputFiles[0].text })
  );
  const leaked = [];
  await page.route('**/api/**', (r) => {
    leaked.push(r.request().url());
    return r.abort();
  });
  const usb = await context.newPage();
  const samples = [];
  let guards;
  try {
    await usb.goto(url + '/#device');
    await usb.addScriptTag({ type: 'module', url: url + '/rf-bench.mjs' });
    await usb.evaluate(() => globalThis.rfBench.setDeviceTransport(null));
    await page.goto('./rf-bench');
    await page.bringToFront();
    await page.getByRole('button', { name: 'Connect RF Ultra', exact: true }).click();
    await page.evaluate(() => globalThis.rfBench.connecting);
    const connected = await page.evaluate(() => globalThis.rfBench.transport.request('/api/device'));
    expect(connected.chip).toBe(before.chip);
    guards = await page.evaluate(async (backup) => {
      const d = new globalThis.rfBench.Device();
      const geometry = {
        sectors: Array.from({ length: 16 }, (_, i) => ({ first: i * 4, count: 4, trailer: i * 4 + 3 }))
      };
      const source = { sector: 0, is_key_b: false, key: backup.keys_a[0] };
      const target = { sector: 1, is_key_b: false };
      let wrongUid = false;
      let refused = false;
      let captures = 0;
      try {
        const card = await d.begin();
        if (JSON.stringify(card.uid) !== JSON.stringify(backup.uid)) {
          throw Error('Wrong card');
        }
        const wrong = [...d.card.uid];
        wrong[0] ^= 1;
        try {
          await d.command(2027, [0x60, 0, ...source.key, wrong.length, ...wrong]);
        } catch (e) {
          if (e.status !== 1) {
            throw e;
          }
          wrongUid = true;
        }
        let blocks = await d.readSector(0, source.key, false, geometry);
        if (JSON.stringify(blocks) !== JSON.stringify(backup.raw_memory.slice(0, 4))) {
          throw Error('Bad data after UID rejection');
        }
        const wrongKey = [...source.key];
        wrongKey[0] ^= 1;
        try {
          await d.readSector(0, wrongKey, false, geometry);
        } catch (e) {
          if (e.status !== 6) {
            throw e;
          }
          refused = true;
        }
        const observations = await d.collectNonces(source, target, 16, geometry);
        captures = observations.length;
        blocks = await d.readSector(0, source.key, false, geometry);
        if (JSON.stringify(blocks) !== JSON.stringify(backup.raw_memory.slice(0, 4))) {
          throw Error('Bad data after captures');
        }
        if (!globalThis.rfBench.transport.capabilities.classicJobs) {
          return { wrongUid, refused, captures, stopMs: null };
        }
        const controller = new AbortController();
        let completed = 0;
        let stoppedAt = 0;
        const body = [1, 2, d.card.uid.length, ...d.card.uid, 0x60, 0, ...source.key, 0x60, 4, 0, 16, 16, 0];
        const terminal = await globalThis.rfBench.classicJob(d.session, body, {
          signal: controller.signal,
          onEvent: (e) => {
            if (e.status) {
              throw Error('Capture failed');
            }
            completed = e.completed;
            if (!e.done && !stoppedAt) {
              stoppedAt = performance.now();
              controller.abort();
            }
          }
        });
        const stopMs = performance.now() - stoppedAt;
        if (!terminal.cancelled || completed < 16 || completed >= 4096) {
          throw Error('Stop not bounded');
        }
        blocks = await d.readSector(0, source.key, false, geometry);
        if (JSON.stringify(blocks) !== JSON.stringify(backup.raw_memory.slice(0, 4))) {
          throw Error('Bad data after Stop');
        }
        return { wrongUid, refused, captures, completed, stopMs, cancelled: terminal.cancelled };
      } finally {
        await d.end();
      }
    }, backup);
    expect(guards.wrongUid && guards.refused).toBe(true);
    expect(guards.captures).toBe(16);
    if (guards.stopMs !== null) {
      expect(guards.cancelled).toBe(true);
      expect(guards.stopMs).toBeLessThan(500);
    }
    for (let round = 0; round < 3; round++) {
      for (const transport of round % 2 ? ['ble', 'usb'] : ['usb', 'ble']) {
        for (const scenario of ['miss', 'hit', 'miss-hit']) {
          for (const mode of round % 2 ? ['read', 'check'] : ['check', 'read']) {
            const target = transport === 'usb' ? usb : page;
            const sample = await target.evaluate(
              async ({ backup, scenario, mode }) => {
                const d = new globalThis.rfBench.Device();
                let count = 0;
                const geometry = {
                  sectors: Array.from({ length: 16 }, (_, i) => ({
                    first: i * 4,
                    count: 4,
                    trailer: i * 4 + 3
                  }))
                };
                try {
                  const card = await d.begin();
                  if (JSON.stringify(card.uid) !== JSON.stringify(backup.uid)) {
                    throw Error('Wrong card');
                  }
                  const started = performance.now();
                  for (let sector = 0; sector < 16; sector++) {
                    for (const hit of scenario === 'miss-hit' ? [false, true] : [scenario === 'hit']) {
                      const key = [...backup.keys_a[sector]];
                      if (!hit) {
                        key[0] ^= 1;
                      }
                      if (mode === 'check') {
                        const found = await d.findKey(sector, false, [key], geometry);
                        if (!!found !== hit) {
                          throw Error('Incorrect key result');
                        }
                      } else {
                        try {
                          const blocks = await d.readSector(sector, key, false, geometry);
                          if (!hit) {
                            throw Error('Wrong key accepted');
                          }
                          if (
                            JSON.stringify(blocks) !==
                            JSON.stringify(backup.raw_memory.slice(sector * 4, sector * 4 + 4))
                          ) {
                            throw Error('Wrong sector data');
                          }
                        } catch (e) {
                          if (hit || e.status !== 6) {
                            throw e;
                          }
                        }
                      }
                      count++;
                    }
                  }
                  return { elapsedMs: performance.now() - started, count };
                } finally {
                  await d.end();
                }
              },
              { backup, scenario, mode }
            );
            samples.push({ round, transport, scenario, mode, ...sample });
          }
        }
      }
    }
    expect(leaked).toEqual([]);
    choose();
  } finally {
    const trace = !page.isClosed() && (await save(ti.outputPath('gatt-trace.json')));
    await writeFile(
      ti.outputPath('samples.json'),
      JSON.stringify({ firmware: before.git, guards, samples }, null, 2)
    );
    await usb.close();
    await page.close();
    await expect.poll(async () => (await (await request.get(url + '/api/device')).json()).busy).toBe(false);
    const after = await (await request.get(url + '/api/device')).json();
    for (const k of ['settings', 'reader', 'active_slot', 'chip', 'git']) {
      expect(after[k]).toEqual(before[k]);
    }
    expect(await usbSlotSnapshot(request, url)).toEqual(slots);
    expect(trace && trace.errors).toEqual([]);
    expect(trace && trace.pending).toEqual([]);
    expect(trace && trace.truncated).toBe(false);
  }
});
