import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';

test('a resource held in memory is not reported as saved and explicit preparation requires storage', async (t) => {
  const { getResource, resourceState, resourceUrl } = await import(
    '../src/runtime/assets.mjs?offline-storage'
  );
  const bytes = new Uint8Array([1, 2, 3, 4]);
  const asset = { size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') };
  const storage = Object.getOwnPropertyDescriptor(globalThis, 'indexedDB');
  Object.defineProperty(globalThis, 'indexedDB', { configurable: true, value: undefined });
  t.after(() => {
    if (storage) {
      Object.defineProperty(globalThis, 'indexedDB', storage);
    } else {
      delete globalThis.indexedDB;
    }
  });
  t.mock.method(globalThis, 'fetch', async () => new Response(bytes));

  const buffer = await getResource(asset, resourceUrl('test'));

  assert.deepEqual(await resourceState(asset), { stored: false, available: false, session: true });
  t.mock.method(globalThis, 'fetch', () => assert.fail('A session resource should not be downloaded again'));
  const phases = [];

  await assert.rejects(
    getResource(asset, resourceUrl('test'), undefined, {
      persistent: true,
      onProgress: (loaded, phase) => phases.push([loaded, phase])
    }),
    /storage could not save/
  );
  assert.deepEqual(phases, [
    [4, 'verify'],
    [4, 'save']
  ]);
  assert.equal(await getResource(asset, resourceUrl('test')), buffer);
  assert.equal((await resourceState(asset)).stored, false);
});

test('resource verification yields to browser tasks and can be cancelled', async () => {
  const { sha256 } = await import('../src/runtime/sha256.mjs');
  const controller = new AbortController();
  const hashing = sha256(new Uint8Array(1024 * 1024), controller.signal);
  setTimeout(() => controller.abort(), 0);

  await assert.rejects(hashing, /Operation cancelled/);
});
