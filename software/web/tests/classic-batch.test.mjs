import test from 'node:test';
import assert from 'node:assert/strict';
import { encodeClassicBatch, decodeClassicBatch } from '../src/device/classic-batch.mjs';

const uid = [1, 2, 3, 4];
const key = [1, 2, 3, 4, 5, 6];
const entries = [
  [0, 0x60, ...key],
  [4, 0x61, ...key]
];

test('a batch correlates every fresh authentication with the submitted key', () => {
  assert.deepEqual(encodeClassicBatch(0, uid, entries), [1, 0, 4, ...uid, 2, ...entries.flat()]);
  const bytes = Uint8Array.from([1, 0, 2, 0, 0, 0, 7, 1, ...key, 0, 6, 0, 7, ...Array(7).fill(0)]);
  assert.deepEqual(
    decodeClassicBatch(bytes, 0, entries).map((x) => x.status),
    [0, 6]
  );
  for (let n = 0; n < bytes.length; n++) {
    assert.throws(() => decodeClassicBatch(bytes.slice(0, n), 0, entries));
  }
  bytes[8] ^= 1;
  assert.throws(() => decodeClassicBatch(bytes, 0, entries), /authentication/);
});

test('a lost tag terminates a batch with the preceding results retained', () => {
  const bytes = Uint8Array.from([1, 0, 1, 0, 1, 0, 0]);
  assert.equal(decodeClassicBatch(bytes, 0, entries)[0].status, 1);
  bytes[4] = 6;
  assert.throws(() => decodeClassicBatch(bytes, 0, entries));
});

test('batched sector geometry and unread masks are validated', () => {
  const entry = [[32, 0x60, ...key]];
  const bytes = Uint8Array.from([1, 1, 1, 0, 0, 1, 2, 255, 255, ...Array(256).fill(0)]);
  assert.equal(decodeClassicBatch(bytes, 1, entry)[0].data.length, 258);
  assert.throws(() => decodeClassicBatch(bytes, 1, entries), /sector/);
  for (const invalid of [[], Array(9).fill(entries[0]), [[40, 0x60, ...key]], [[0, 0x62, ...key]]]) {
    assert.throws(() => encodeClassicBatch(1, uid, invalid));
  }
});
