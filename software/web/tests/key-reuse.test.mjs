import test from 'node:test';
import assert from 'node:assert/strict';
import { createKeyReuseFilter } from '../src/recovery/key-reuse.mjs';

const key = (byte) => Array(6).fill(byte);
const target = { sector: 1, is_key_b: true };
const summary = {
  prng: 'Hard',
  mapping: [
    { key_a: key(1), key_b: key(2) },
    { key_a: key(3), key_b: null }
  ]
};
const observations = [
  { nt1: 1, ciphertext: 2, parity: [0, 0, 0, 0] },
  { nt1: 3, ciphertext: 4, parity: [1, 0, 0, 0] }
];

function fixture(options = {}) {
  const captures = [];
  const requests = [];
  const filter = createKeyReuseFilter({
    uid: () => [1, 2, 3, 4],
    collect: async (...args) => {
      captures.push(args);
      if (options.failure) {
        throw options.failure;
      }
      return observations;
    },
    call: async (request) => {
      requests.push(request);
      return { candidates: [] };
    }
  });
  return { filter, captures, requests };
}

test('reuse screens several keys from one two-nonce capture per target', async () => {
  const { filter, captures, requests } = fixture();
  assert.equal(await filter.test(summary, target, key(4)), false);
  assert.equal(await filter.test(summary, target, key(5)), false);
  assert.equal(captures.length, 1);
  assert.deepEqual(captures[0], [{ sector: 1, is_key_b: false, key: key(3) }, target, 2]);
  assert(requests.every((request) => request.weak_prng === false));
  assert(requests.every((request) => request.observations === observations));
  assert.deepEqual(filter.take(target), observations);
  assert.deepEqual(filter.take(target), [], 'dictionary evidence is consumed once');
});

test('reuse retains physical checks for unknown/static PRNGs and few distinct keys', async () => {
  const { filter, captures } = fixture();
  for (const prng of ['Unknown', 'Static']) {
    assert.equal(await filter.test({ ...summary, prng }, target, key(4)), null);
  }
  const repeated = { prng: 'Hard', mapping: summary.mapping.map(() => ({ key_a: key(1), key_b: key(2) })) };
  assert.equal(await filter.test(repeated, target, key(4)), null);
  assert.equal(captures.length, 0);
});

test('readable Key B is never used as a nonce source', async () => {
  const { filter, captures } = fixture();
  const readable = structuredClone(summary);
  readable.mapping[1] = { key_a: null, key_b: key(3), origin_b: 'Trailer' };
  await filter.test(readable, target, key(4));
  assert.deepEqual(captures[0][0], { sector: 0, is_key_b: false, key: key(1) });
  readable.mapping[0] = { key_a: null, key_b: key(2), origin_b: 'Trailer' };
  readable.mapping.push({ key_a: null, key_b: key(1), origin_b: 'Trailer' });
  assert.equal(await filter.test(readable, target, key(4)), null);
});

for (const status of [6, 0x69]) {
  test(`unavailable captures disable filtering for just that target (${status})`, async () => {
    const { filter, captures } = fixture({ failure: Object.assign(new Error('Unavailable'), { status }) });
    assert.equal(await filter.test(summary, target, key(4)), null);
    assert.equal(await filter.test(summary, target, key(5)), null);
    assert.equal(captures.length, 1);
    assert.equal(filter.eligible(summary, { ...target, is_key_b: false }), true);
  });
}

test('card loss and cancellation propagate instead of becoming local misses', async () => {
  for (const failure of [
    Object.assign(new Error('Card lost'), { status: 1 }),
    new Error('Operation cancelled')
  ]) {
    const { filter } = fixture({ failure });
    await assert.rejects(filter.test(summary, target, key(4)), (error) => error === failure);
  }
});

test('failed recovery drops nonce evidence and restores physical reuse checks', async () => {
  const { filter } = fixture();
  await filter.test(summary, target, key(4));
  filter.disable(target);
  assert.deepEqual(filter.take(target), []);
  assert.equal(filter.eligible(summary, target), false);
  assert.equal(await filter.test(summary, target, key(5)), null);
  assert.equal(fixture().filter.eligible(summary, target), true, 'a new read starts with fresh evidence');
});
