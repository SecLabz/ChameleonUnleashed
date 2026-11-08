import { createClient } from './nfc-wasm/index.mjs';
import { HARDNESTED } from './hardnested-build.mjs';
import { getResource, resourceUrl } from './assets.mjs';
import { appUrl } from './environment.mjs';

export const HARDNESTED_URL = resourceUrl('hardnested/');

export function supportsSimd() {
  // Minimal v128-returning function; avoid compiling the full engine twice.
  // biome-ignore format: Keep the probe bytes grouped by WebAssembly section.
  const simdProbe = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, // Header and version.
    1, 5, 1, 96, 0, 1, 123,     // Function type returning v128.
    3, 2, 1, 0,                 // Function declaration.
    10, 22, 1, 20, 0, 253, 12,  // Body: v128.const followed by 16 zero bytes.
    ...Array(16).fill(0), 11
  ]);

  return WebAssembly.validate(simdProbe);
}

export async function createHardnestedRuntime(
  signal,
  workerFactory = () => new Worker(appUrl('worker.mjs'), { type: 'module' })
) {
  const variants = supportsSimd() ? [HARDNESTED.simd, HARDNESTED.scalar] : [HARDNESTED.scalar];

  for (const asset of variants) {
    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }

    let client;

    try {
      const bytes = await getResource(asset, HARDNESTED_URL + asset.path, signal);
      client = await createClient(bytes, { signal, workerFactory });
      return client;
    } catch {
      client?.dispose();

      if (signal?.aborted) {
        throw new Error('Operation cancelled');
      }
    }
  }

  throw new Error('Hardnested module unavailable. Connect to the Internet and try again.');
}
