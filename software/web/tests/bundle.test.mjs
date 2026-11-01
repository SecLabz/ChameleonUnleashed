import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { Worker } from 'node:worker_threads';
import { once } from 'node:events';
import { CORE } from '../../../.cache/web/assets/modules/runtime/core-build.mjs';

const site = new URL('../../../.cache/web/assets/site/', import.meta.url);

test('the bundled app has no module waterfall and its Worker runs the core', {
  timeout: 10000
}, async (t) => {
  const html = await readFile(new URL('index.html', site), 'utf8');

  assert.equal((html.match(/<script\b/g) || []).length, 1);
  assert.match(html, /<script type="module" src="\/app.mjs">/);

  for (const name of ['app.mjs', 'worker.mjs']) {
    const metadata = JSON.parse(await readFile(new URL(`../${name}.json`, site), 'utf8'));
    const outputs = Object.values(metadata.outputs);

    assert.equal(outputs.length, 1);
    assert.deepEqual(outputs[0].imports, []);
  }

  const bytes = await readFile('.cache/web/cdn/core/' + CORE.path);
  const worker = new Worker(
    `
    const { parentPort, workerData } = require('node:worker_threads');

    globalThis.self = globalThis;
    self.postMessage = data => parentPort.postMessage(data);

    import(workerData).then(() => {
      parentPort.on('message', data => self.onmessage({ data }));
    });
  `,
    { eval: true, workerData: new URL('worker.mjs', site).href }
  );
  t.after(() => worker.terminate());

  const init = once(worker, 'message');
  worker.postMessage({ id: 1, wasm: bytes });

  assert.deepEqual((await init)[0], { id: 1, value: null });

  const geometry = once(worker, 'message');
  worker.postMessage({ id: 2, request: { op: 'classic_geometry', sak: 8 } });

  const result = (await geometry)[0];

  assert.equal(result.id, 2);
  assert.equal(result.value.blocks, 64);
});
