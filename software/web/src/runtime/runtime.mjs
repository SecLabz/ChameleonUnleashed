import { createClient } from './nfc-wasm/index.mjs';
import { CORE } from './core-build.mjs';
import { getResource, resourceUrl } from './assets.mjs';

export const CORE_URL = resourceUrl('core/' + CORE.path);

export async function createRuntime(
  signal,
  workerFactory = () => new Worker('/worker.mjs', { type: 'module' })
) {
  let bytes;

  try {
    bytes = await getResource(CORE, CORE_URL, signal);
  } catch (error) {
    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }

    throw new Error('Core module unavailable. Connect to the Internet and try again.', { cause: error });
  }

  return createClient(bytes, { workerFactory, signal });
}

export async function withRuntime(work, signal) {
  const engine = await createRuntime(signal);
  try {
    return await work((request) => engine.request(request, { signal }));
  } finally {
    engine.dispose();
  }
}
