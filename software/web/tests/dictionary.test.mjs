import test from 'node:test';
import assert from 'node:assert/strict';
import { filterDictionary } from '../src/recovery/dictionary.mjs';

test('dictionary filtering collects additional evidence before returning candidates', async () => {
  const keys = [Array(6).fill(255)];
  const result = { next_count: 0, candidates: keys, exhausted: true };
  const batches = [];
  const observations = [];
  const filtered = await filterDictionary({
    uid: [1, 2, 3, 4],
    keys,
    collect: async (count) => {
      batches.push(count);
      return Array.from({ length: count }, () => ({ nt1: batches.length }));
    },
    call: async (request) => {
      if (request.op === 'recovery_plan') {
        return { dictionary_start_count: 3 };
      }
      assert.equal(request.op, 'filter_dictionary');
      assert.deepEqual(request.keys, keys);
      observations.push(structuredClone(request.observations));
      return observations.length === 1 ? { next_count: 1, candidates: [] } : result;
    }
  });
  assert.equal(filtered, result);
  assert.deepEqual(batches, [3, 1]);
  assert.deepEqual(
    observations.map((records) => records.length),
    [3, 4]
  );
  assert.deepEqual(observations[1].slice(0, 3), observations[0]);
});

test('dictionary filtering stops when capture fails instead of using incomplete evidence', async () => {
  const failure = new Error('Operation cancelled');
  await assert.rejects(
    filterDictionary({
      uid: [1, 2, 3, 4],
      keys: [],
      collect: async () => {
        throw failure;
      },
      call: async (request) => {
        assert.equal(request.op, 'recovery_plan');
        return { dictionary_start_count: 3 };
      }
    }),
    (error) => error === failure
  );
});
