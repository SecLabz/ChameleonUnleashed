import test from 'node:test';
import assert from 'node:assert/strict';
import {
  st25tbIdentity,
  st25tbName,
  encodeSt25tbTearoff,
  decodeSt25tbTearoff
} from '../src/device/st25tb-protocol.mjs';
import { Device } from '../src/device/device.mjs';

const uid = [0xd0, 2, 0x33, 1, 2, 3, 4, 5];

const stateBytes = () =>
  Uint8Array.from([3, 0, 5, 3, 0x78, 0x56, 0x34, 0x12, 0x98, 0xba, 0xdc, 0xfe, 9, 0, 0, 0, 130, 0, 2, 0]);

test('family identity preserves geometry and product names', () => {
  assert.deepEqual(st25tbIdentity(Uint8Array.from([...uid, 0x22, 16, 4])), {
    protocol: 'st25tb',
    uid,
    chip_id: 0x22,
    blocks: 16,
    block_size: 4
  });
  assert.equal(st25tbIdentity(Uint8Array.from([...uid, 0x22, 15, 4])), null);
  assert.equal(st25tbIdentity(Uint8Array.from([...uid, 0x22, 16, 8])), null);

  for (const [product, name] of [
    [0x33, 'ST25TB512-AT'],
    [0x1b, 'SRI512 / ST25TB512-AC'],
    [0x18, 'SRI512'],
    [0x1a, 'SRI512'],
    [0x1c, 'ST25TB/SRx']
  ]) {
    assert.equal(st25tbName([0xd0, 2, product]), name);
  }
});

test('HTTP tearoff formats retain independent little-endian request and response vectors', () => {
  assert.deepEqual([...encodeSt25tbTearoff(uid, 5, 0xfedcba98)], [...uid, 5, 0x98, 0xba, 0xdc, 0xfe]);
  assert.deepEqual(decodeSt25tbTearoff(stateBytes()), {
    state: 3,
    outcome: 0,
    block: 5,
    flags: 3,
    current: 0x12345678,
    target: 0xfedcba98,
    attempts: 9,
    delay_us: 130,
    seconds: 2
  });

  const padded = new Uint8Array(24);
  padded.set(stateBytes(), 2);

  assert.deepEqual(decodeSt25tbTearoff(padded.subarray(2, 22)), decodeSt25tbTearoff(stateBytes()));
  assert.throws(() => decodeSt25tbTearoff(stateBytes().slice(0, -1)), /Invalid tearoff state/);

  for (const [offset, value] of [
    [0, 4],
    [1, 6]
  ]) {
    const data = stateBytes();
    data[offset] = value;

    assert.throws(() => decodeSt25tbTearoff(data), /Invalid tearoff state/);
  }
});

test('Device keeps its HTTP endpoints, session guard and public methods', async () => {
  const previous = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (path, options) => {
    calls.push({ path, body: [...options.body] });

    return new Response(path.includes('/state?') ? stateBytes() : new Uint8Array());
  };

  try {
    const device = new Device();

    await assert.rejects(device.tearoffStart(uid, 5, 0xfedcba98), /No device session/);
    assert.equal(calls.length, 0);

    device.session = 17;
    await device.tearoffStart(uid, 5, 0xfedcba98);

    assert.equal((await device.tearoffState()).current, 0x12345678);

    await device.tearoffCancel();

    assert.deepEqual(calls, [
      { path: '/api/st25tb/tearoff/start?id=17', body: [...uid, 5, 0x98, 0xba, 0xdc, 0xfe] },
      { path: '/api/st25tb/tearoff/state?id=17', body: [] },
      { path: '/api/st25tb/tearoff/cancel?id=17', body: [] }
    ]);
  } finally {
    globalThis.fetch = previous;
  }
});
