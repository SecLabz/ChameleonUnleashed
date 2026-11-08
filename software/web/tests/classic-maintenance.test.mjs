import { Device } from '../../../.cache/web/assets/modules/device/device.mjs';
import { maintenanceMethod } from '../src/documents/classic-magic.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { inspectClassic } from '../../../.cache/web/assets/modules/device/classic-inspect.mjs';
import { repairBlock0, factoryTrailer, maintenanceSectors } from '../src/documents/classic-maintenance.mjs';
import { createEngine } from '../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import { readFile } from 'node:fs/promises';

const card = { uid: [1, 2, 3, 4], atqa: [0, 4], sak: 8 };

test('scan confirms a CUID in one bound inspection without memory or recovery commands', async () => {
  const calls = [];
  const device = {
    async command(command, request) {
      calls.push(command);
      assert.equal(command, 2040);
      assert.deepEqual(request.slice(0, 14), [4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 4, 0, 8]);
      assert.ok(request[14] > 0 && request[14] <= 64);
      assert.equal(request.length, 15 + 6 * request[14]);
      assert.deepEqual(request.slice(15, 21), Array(6).fill(255));
      return Uint8Array.from([0, 4, 3, 1, 1, ...Array(6).fill(255)]);
    }
  };
  const observation = await inspectClassic(device, card);
  assert.equal(observation.magic.flags, 4);
  assert.equal(observation.prng, 'Weak');
  assert.deepEqual(observation.credentials, [{ key: Array(6).fill(255), keyB: false }]);
  assert.deepEqual(calls, [2040]);
});

test('inspection retains backdoor evidence without inventing credentials and preserves unsupported identity', async () => {
  const device = {
    async command() {
      return Uint8Array.from([0, 2, 0, 2, 0, ...Array(6).fill(0)]);
    }
  };
  const observation = await inspectClassic(device, card);
  assert.equal(observation.magic.flags, 2);
  assert.equal(observation.prng, 'Hard');
  assert.deepEqual(observation.credentials, []);
  device.command = async () => {
    throw Object.assign(new Error('unsupported'), { status: 0x67 });
  };
  assert.equal((await inspectClassic(device, card)).unavailable, true);
  assert.deepEqual(observation.card, card);
});

test('inspection rejects malformed metadata, unsubmitted credentials and cancelled operations', async () => {
  for (const reply of [
    [],
    [0, 4, 3, 3, 0, ...Array(6).fill(0)],
    [0, 4, 3, 1, 3, ...Array(6).fill(255)],
    [0, 4, 3, 1, 0, ...Array(6).fill(255)],
    [0, 4, 3, 1, 1, ...Array(6).fill(42)]
  ]) {
    await assert.rejects(inspectClassic({ command: async () => Uint8Array.from(reply) }, card));
  }
  const signal = AbortSignal.abort();
  await assert.rejects(
    inspectClassic(
      {
        signal,
        command: async () => {
          throw Object.assign(new Error('cancelled'), { status: 0x67 });
        }
      },
      card
    ),
    /cancelled/
  );
});

test('block-0 repair changes only identity bytes and respects UID layout and manufacturer tail', () => {
  const before = Array.from({ length: 16 }, (_, index) => index);
  const geometry = { sak: 8, atqa: [0, 4] };
  const fixed = repairBlock0(before, card.uid, geometry);
  assert.deepEqual(fixed.slice(0, 8), [1, 2, 3, 4, 4, 8, 4, 0]);
  assert.deepEqual(fixed.slice(8), before.slice(8));
  const seven = repairBlock0(before, [4, 1, 2, 3, 4, 5, 6], geometry);
  assert.deepEqual(seven.slice(7, 10), [8, 0x44, 0]);
  assert.deepEqual(seven.slice(10), before.slice(10));
});

test('2K repair preserves observed SL1 or SL2 SAK using production geometry', async () => {
  const before = Array.from({ length: 16 }, (_, index) => index);
  const call = await createEngine(
    await readFile(
      new URL('../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
    )
  );
  const geometry = call({ op: 'classic_geometry', sak: 16 });
  assert.equal(geometry.blocks, 128);
  assert.equal(geometry.sak, 8, 'selector 16 selects 2K, whose canonical SL1 SAK is 08');
  assert.equal(repairBlock0(before, card.uid, geometry, 8)[5], 8);
  assert.equal(repairBlock0(before, card.uid, geometry, 16)[5], 16);
  assert.equal(repairBlock0(before, card.uid, geometry)[5], 8);
  assert.deepEqual(repairBlock0(before, [4, 1, 2, 3, 4, 5, 6], geometry, 8).slice(7, 10), [8, 0x44, 0]);
  assert.throws(() => repairBlock0(before, card.uid, geometry, 24), /SAK/);
});

test('factory trailers and explicit repair sector selection are distinct', () => {
  assert.deepEqual(factoryTrailer().slice(6, 10), [255, 7, 128, 105]);
  assert.deepEqual(factoryTrailer(true).slice(6, 10), [255, 7, 128, 188]);
  assert.deepEqual(maintenanceSectors('', 16), []);
  assert.deepEqual(maintenanceSectors('0, 2,2', 16), [0, 2]);
  assert.equal(maintenanceSectors('all', 40).at(-1), 39);
  for (const text of ['-1', '16', '0,,1', 'no']) {
    assert.throws(() => maintenanceSectors(text, 16), /sector indices/);
  }
  assert.equal(maintenanceMethod({ flags: 2 }), 2);
  assert.equal(maintenanceMethod({ flags: 4 }), 3);
  assert.equal(maintenanceMethod({ flags: 9 }), null);
});

test('maintenance rejects a mismatching response UID without changing the bound target', async () => {
  const device = new Device();
  device.command = async () => Uint8Array.from([4, 9, 2, 3, 4, 0, 0, 0, 4, 0, 8, ...Array(16).fill(0)]);
  const options = { method: 3, model: 1, uid: [1, 2, 3, 4] };
  await assert.rejects(
    device.maintenanceUnit(options, 0, { key: Array(6).fill(255), keyB: false }),
    /target changed/
  );
  assert.deepEqual(options.uid, [1, 2, 3, 4]);
});

test('checked 2K maintenance requires its flag on reads and mutations without a legacy retry', async () => {
  const options = { method: 3, model: 6, uid: [1, 2, 3, 4], capacityCheck: true };
  const credential = { key: Array(6).fill(255), keyB: false };
  for (const [change, check, expected] of [
    [null, false, 20],
    [{ before: Array(16).fill(0), after: Array(16).fill(42) }, true, 52],
    [{ before: Array(16).fill(0), after: Array(16).fill(42) }, false, 52]
  ]) {
    const device = new Device();
    let calls = 0;
    const reject = async (number, request) => {
      calls++;
      assert.equal(number, 2039);
      assert.equal(request.length, expected);
      assert.equal(request.at(-1), 1);
      throw Object.assign(new Error('old firmware rejects checked maintenance'), { status: 0x60 });
    };
    device.command = reject;
    device.writeCommand = reject;
    await assert.rejects(device.maintenanceUnit(options, 126, credential, change, check), /old firmware/);
    assert.equal(calls, 1);
  }
});
