import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { CORE } from '../../../.cache/web/assets/modules/runtime/core-build.mjs';
import { CORE_URL, createRuntime } from '../../../.cache/web/assets/modules/runtime/runtime.mjs';

const wasm = await readFile('.cache/web/cdn/core/' + CORE.path);
const buffer = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength);
const workers = [];

function workerFactory() {
  const worker = {
    closed: false,

    terminate() {
      this.closed = true;
    },

    postMessage({ id, wasm }) {
      assert.deepEqual(new Uint8Array(wasm), new Uint8Array(buffer));

      queueMicrotask(() => this.onmessage({ data: { id, value: null } }));
    }
  };
  workers.push(worker);

  return worker;
}

test('CDN core validates resources before creating Workers and reuses verified downloads', async (t) => {
  const originalFetch = globalThis.fetch;

  try {
    await t.test('HTTP errors and corrupt or truncated downloads create no Worker', async () => {
      for (const response of [
        () => new Response(null, { status: 404 }),
        () => new Response(wasm.subarray(0, wasm.length - 1)),
        () => new Response(new Uint8Array(CORE.size))
      ]) {
        globalThis.fetch = async (url) => {
          assert.equal(url, CORE_URL);

          return response();
        };

        await assert.rejects(createRuntime(undefined, workerFactory), /Core module unavailable/);
        assert.equal(workers.length, 0);
      }
    });

    await t.test('cancellation aborts a pending network download', async () => {
      const controller = new AbortController();
      globalThis.fetch = (_, { signal }) =>
        new Promise((resolve, reject) => {
          signal.addEventListener('abort', () => reject(new Error('Aborted')), { once: true });
          controller.abort();
        });

      await assert.rejects(createRuntime(controller.signal, workerFactory), /cancelled/);
      assert.equal(workers.length, 0);
    });

    await t.test('the resource deadline covers a stalled response body', async (t) => {
      t.mock.timers.enable({ apis: ['setTimeout'] });

      let started;
      const reading = new Promise((resolve) => {
        started = resolve;
      });
      globalThis.fetch = async (_, { signal }) => ({
        ok: true,
        headers: new Headers(),
        body: {
          getReader() {
            return {
              read() {
                started();

                return new Promise((resolve, reject) =>
                  signal.addEventListener('abort', () => reject(new Error('Aborted body')), { once: true })
                );
              }
            };
          }
        }
      });

      const rejected = assert.rejects(createRuntime(undefined, workerFactory), /Core module unavailable/);
      await reading;
      t.mock.timers.tick(10000);
      await rejected;

      assert.equal(workers.length, 0);
    });

    await t.test('a valid download is reused without another network request', async () => {
      let requests = 0;
      globalThis.fetch = async (url) => {
        assert.equal(url, CORE_URL);

        requests++;

        return new Response(wasm);
      };

      const first = await createRuntime(undefined, workerFactory);
      first.dispose();
      globalThis.fetch = async () => {
        throw new Error('Offline');
      };

      const second = await createRuntime(undefined, workerFactory);
      second.dispose();

      assert.equal(requests, 1);
    });

    await t.test('the core binary is absent from the embedded site', async () => {
      const sizes = JSON.parse(await readFile('.cache/web/assets/sizes.json', 'utf8'));

      assert(Object.keys(sizes).every((path) => !path.endsWith('.wasm')));
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});
