import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import {
  createEngine,
  hardnestedTablesRequest
} from '../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';
import { HARDNESTED } from '../../../.cache/web/assets/modules/runtime/hardnested-build.mjs';

const originalFetch = globalThis.fetch;
const validate = WebAssembly.validate;
const core = await readFile('.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm');
const modules = Object.fromEntries(
  await Promise.all(
    Object.entries(HARDNESTED).map(async ([name, asset]) => [
      name,
      await readFile('.cache/web/cdn/hardnested/' + asset.path)
    ])
  )
);

const workers = [];
const loaded = [];

function workerFactory() {
  const worker = {
    closed: false,

    terminate() {
      this.closed = true;
    },

    postMessage({ id, wasm, request }) {
      queueMicrotask(async () => {
        try {
          if (wasm) {
            loaded.push(createHash('sha256').update(new Uint8Array(wasm)).digest('hex'));
            this.engine = await createEngine(wasm);
          }

          const value = wasm ? null : this.engine(request);
          if (!this.closed) {
            this.onmessage({ data: { id, value } });
          }
        } catch (error) {
          if (!this.closed) {
            this.onmessage({ data: { id, error: error.message } });
          }
        }
      });
    }
  };
  workers.push(worker);

  return worker;
}

test('core and optional module have separate request boundaries', async () => {
  const request = await createEngine(core);

  assert.equal(request({ op: 'classic_geometry', sak: 8 }).blocks, 64);
  assert.throws(() =>
    request({ op: 'hardnested_new', uid: [1, 2, 3, 4], block: 4, key_b: true, budget: 32 })
  );

  for (const bytes of Object.values(modules)) {
    const hardnested = await createEngine(bytes);

    assert.throws(() => hardnested({ op: 'classic_geometry', sak: 8 }));

    const pack = new Uint8Array([75, 72, 78, 80, 1, 0, 0, 0, 12, 0, 0, 0]).buffer;

    assert.equal(hardnested(hardnestedTablesRequest(pack)).loaded, true);

    const { job } = hardnested({
      op: 'hardnested_new',
      uid: [1, 2, 3, 4],
      block: 4,
      key_b: true,
      budget: 32
    });

    assert.equal(hardnested({ op: 'hardnested_dispose', job }).disposed, true);
  }
});

test('optional loader verifies CDN downloads, selects scalar without SIMD, caches downloads and cancels', async () => {
  const { createHardnestedRuntime } = await import(
    '../../../.cache/web/assets/modules/runtime/hardnested-runtime.mjs'
  );
  let client;
  let requests = [];

  try {
    WebAssembly.validate = () => false;
    for (const response of [
      () => new Response(null, { status: 404 }),
      () => new Response(modules.scalar.subarray(0, modules.scalar.length - 1)),
      () => new Response(new Uint8Array(HARDNESTED.scalar.size).fill(0))
    ]) {
      requests = [];
      globalThis.fetch = async (url) => {
        requests.push(url);

        return response();
      };

      await assert.rejects(createHardnestedRuntime(undefined, workerFactory), /module unavailable.*Internet/);
      assert.equal(requests.length, 1);
      assert(requests[0].endsWith('hardnested-scalar.wasm'));
      assert.equal(workers.length, 0);
    }
    // Abort an actual pending resource request; no Worker can be left behind.
    const controller = new AbortController();
    globalThis.fetch = async (url, { signal }) =>
      new Promise((resolve, reject) => {
        signal.addEventListener('abort', () => reject(new Error('Aborted')), { once: true });
        controller.abort();
      });

    await assert.rejects(createHardnestedRuntime(controller.signal, workerFactory), /cancelled/);
    assert.equal(workers.length, 0);

    requests = [];
    globalThis.fetch = async (url) => {
      requests.push(url);

      return new Response(modules.scalar);
    };
    client = await createHardnestedRuntime(undefined, workerFactory);

    assert.equal(requests.length, 1);
    assert(requests[0].endsWith('hardnested-scalar.wasm'));

    client.dispose();
    client = null;
    // A verified CDN download is reused without a manual import or network request.
    requests = [];
    globalThis.fetch = async (url) => {
      requests.push(url);
      throw new Error('Offline');
    };
    client = await createHardnestedRuntime(undefined, workerFactory);
    client.dispose();
    client = null;

    assert.deepEqual(requests, []);
    // SIMD is preferred when supported; a failed SIMD request falls back to scalar.
    WebAssembly.validate = () => true;
    requests = [];
    globalThis.fetch = async (url) => {
      requests.push(url);
      throw new Error('Offline');
    };
    client = await createHardnestedRuntime(undefined, workerFactory);

    assert(requests.some((url) => url.endsWith('hardnested-simd.wasm')));

    client.dispose();
    client = null;
    requests = [];
    globalThis.fetch = async (url) => {
      requests.push(url);

      return new Response(modules.simd);
    };
    client = await createHardnestedRuntime(undefined, workerFactory);

    assert.equal(requests.length, 1);
    assert(requests[0].endsWith('hardnested-simd.wasm'));
    assert.equal(loaded.at(-1), HARDNESTED.simd.sha256);
  } finally {
    globalThis.fetch = originalFetch;
    WebAssembly.validate = validate;
    client?.dispose();

    assert(workers.every((worker) => worker.closed));
  }
});
