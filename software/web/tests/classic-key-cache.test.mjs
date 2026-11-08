import test from 'node:test';
import assert from 'node:assert/strict';
import { createClassicKeyCache } from '../src/recovery/classic-key-cache.mjs';

const card = { uid: [1, 2, 3, 4], sak: 8, atqa: [0, 4] };
const geometry = { blocks: 64, sectors: Array(16).fill(null) };
const mapping = () =>
  Array.from({ length: 16 }, () => ({
    key_a: Array(6).fill(255),
    key_b: Array(6).fill(0),
    origin_b: 'Trailer'
  }));

function fixture() {
  const values = new Map();
  let writes = 0;
  const store = {
    get length() {
      return values.size;
    },
    key: (index) => [...values.keys()][index],
    getItem: (key) => values.get(key) ?? null,
    setItem(key, value) {
      writes++;
      values.set(key, value);
    },
    removeItem(key) {
      writes++;
      values.delete(key);
    }
  };
  return { cache: createClassicKeyCache(() => store), store, values, writes: () => writes };
}

test('keys persist independently of documents and unchanged reads perform no write', () => {
  const { cache, store, values, writes } = fixture();
  const keys = mapping();
  assert(cache.save(card, geometry, keys));
  assert.equal(writes(), 1);
  assert(cache.save(card, geometry, keys));
  assert.equal(writes(), 1);
  const restored = createClassicKeyCache(() => store).load(card, geometry);
  assert.deepEqual(restored, keys);
  restored[0].key_a[0] = 1;
  assert.equal(cache.load(card, geometry)[0].key_a[0], 255);
  assert(![...values.values()][0].includes('memory'));
  assert.deepEqual(cache.state(), { available: true, count: 1 });
});

test('the full UID, observed identity and geometry isolate entries', () => {
  const { cache } = fixture();
  cache.save(card, geometry, mapping());
  for (const other of [
    { ...card, uid: [5, 2, 3, 4] },
    { ...card, uid: [9, 8, 7, 1, 2, 3, 4] },
    { ...card, sak: 24 },
    { ...card, atqa: [0, 2] }
  ]) {
    assert.deepEqual(cache.load(other, geometry), []);
  }
  assert.deepEqual(cache.load(card, { blocks: 128, sectors: Array(32).fill(null) }), []);
  assert(!cache.save({ ...card, uid: [256, 2, 3, 4] }, geometry, mapping()));
  assert(!cache.save(card, { blocks: 256, sectors: Array(16) }, mapping()));
  for (const [blocks, count] of [
    [20, 5],
    [128, 32],
    [256, 40]
  ]) {
    const large = { blocks, sectors: Array(count).fill(null) };
    const keys = Array.from({ length: count }, () => ({ key_a: null, key_b: null }));
    keys[count - 1] = { key_a: Array(6).fill(0), key_b: Array(6).fill(42) };
    assert(cache.save(card, large, keys));
    assert.deepEqual(cache.load(card, large)[count - 1].key_a, Array(6).fill(0));
  }
  assert.equal(cache.state().count, 4);
});

test('partial and all-zero keys are kept; empty or malformed keys cannot create a tag', () => {
  const { cache, writes } = fixture();
  const keys = Array.from({ length: 16 }, () => ({ key_a: null, key_b: null }));
  keys[0].key_a = [1, 2];
  keys[1].key_b = Array(6).fill(null);
  cache.save(card, geometry, keys);
  assert.equal(writes(), 0);
  keys[15].key_b = Array(6).fill(0);
  cache.save(card, geometry, keys);
  assert.deepEqual(cache.load(card, geometry)[15], {
    key_a: null,
    key_b: Array(6).fill(0),
    origin_b: null
  });
  keys[15].key_b = null;
  cache.save(card, geometry, keys);
  assert.deepEqual(cache.load(card, geometry), []);
  assert.equal(cache.state().count, 0);
});

test('corrupt entries are ignored and clearing preserves other browser data', () => {
  const { cache, values } = fixture();
  cache.save(card, geometry, mapping());
  const id = [...values.keys()][0];
  const original = JSON.parse(values.get(id));
  for (const text of [
    '{',
    'x'.repeat(12001),
    JSON.stringify({ ...original, version: 2 }),
    JSON.stringify({ ...original, keys: [{ key_a: 'FFFFFFFFFFFF' }] })
  ]) {
    values.set(id, text);
    assert.deepEqual(cache.load(card, geometry), []);
    assert.equal(cache.state().count, 0);
  }
  values.set('chameleon-dictionary-v1', 'A0A1A2A3A4A5');
  values.set('chameleon-studio-v1', 'draft');
  values.set('other-app', 'keep');
  assert(cache.clear());
  assert.deepEqual([...values.keys()], ['chameleon-dictionary-v1', 'chameleon-studio-v1', 'other-app']);
});

test('unavailable storage and quota errors never escape into reading', () => {
  const unavailable = createClassicKeyCache(() => {
    throw new Error('SecurityError');
  });
  assert.deepEqual(unavailable.load(card, geometry), []);
  assert.equal(unavailable.save(card, geometry, mapping()), false);
  assert.deepEqual(unavailable.state(), { available: false, count: 0 });
  assert.equal(unavailable.clear(), false);
  const quota = createClassicKeyCache(() => ({
    getItem: () => null,
    setItem() {
      throw new Error('QuotaExceededError');
    }
  }));
  assert.equal(quota.save(card, geometry, mapping()), false);
});
