import test from 'node:test';
import assert from 'node:assert/strict';
import { decodeClassicJob } from '../src/device/classic-job.mjs';

function event(kind, completed, data, flags = 0, status = 0) {
  return Uint8Array.from([
    1,
    0,
    0,
    0,
    9,
    0,
    0,
    flags,
    kind,
    status >> 8,
    status & 255,
    completed >> 8,
    completed & 255,
    ...data
  ]);
}

test('capture events validate exact fresh observation counts and wire parity', () => {
  const bytes = event(2, 16, Array(192).fill(0));
  assert.equal(decodeClassicJob(bytes, 9, 0, 2).completed, 16);
  assert.throws(() => decodeClassicJob(bytes, 9, 0, 2, 16));
  bytes[13 + 8] = 2;
  assert.throws(() => decodeClassicJob(bytes, 9, 0, 2), /capture/);
});

test('cancellation and RF loss cannot fabricate additional retained observations', () => {
  assert.equal(decodeClassicJob(event(2, 16, [], 3), 9, 0, 2, 16).cancelled, true);
  assert.throws(() => decodeClassicJob(event(2, 17, [], 3), 9, 0, 2, 16));
  assert.equal(decodeClassicJob(event(2, 16, [], 1, 1), 9, 0, 2, 16).status, 1);
  assert.throws(() => decodeClassicJob(event(2, 16, [0], 1, 1), 9, 0, 2, 16));
});

test('sector and authentication events preserve error and unread-block boundaries', () => {
  assert.equal(decodeClassicJob(event(1, 1, [], 0, 6), 9, 0, 1).status, 6);
  assert.throws(() => decodeClassicJob(event(1, 1, Array(66).fill(0), 0, 6), 9, 0, 1));
  assert.equal(decodeClassicJob(event(1, 1, [0, 3, ...Array(64).fill(0)]), 9, 0, 1).data[1], 3);
  assert.throws(() => decodeClassicJob(event(1, 1, [1, 0, ...Array(64).fill(0)]), 9, 0, 1));
  assert.equal(decodeClassicJob(event(0, 1, [1, ...Array(6).fill(255)]), 9, 0, 0).status, 0);
  assert.throws(() => decodeClassicJob(event(0, 1, Array(7).fill(0)), 9, 0, 0));
});
