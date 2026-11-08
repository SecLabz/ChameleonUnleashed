import { CORE } from './core-build.mjs';
import { CORE_URL } from './runtime.mjs';
import { HARDNESTED } from './hardnested-build.mjs';
import { HARDNESTED_URL, supportsSimd } from './hardnested-runtime.mjs';
import { getResource, resourceState, TABLE, TABLE_URL } from './assets.mjs';
import { shellState, prepareShell } from './shell.mjs';

function resources() {
  const entries = [
    { asset: CORE, url: CORE_URL },
    { asset: HARDNESTED.scalar, url: HARDNESTED_URL + HARDNESTED.scalar.path },
    { asset: TABLE, url: TABLE_URL }
  ];

  if (supportsSimd()) {
    entries.push({ asset: HARDNESTED.simd, url: HARDNESTED_URL + HARDNESTED.simd.path });
  }

  return entries;
}

export async function offlineState(signal) {
  const modules = await Promise.all(
    resources().map(async (entry) => ({ ...entry, ...(await resourceState(entry.asset, signal)) }))
  );
  return [...(await shellState()), ...modules];
}

export async function prepareOffline(signal, onProgress) {
  const entries = await offlineState(signal);
  const total = entries.reduce((sum, entry) => sum + entry.asset.size, 0);
  let completed = 0;

  for (const entry of entries) {
    if (signal.aborted) {
      throw new Error('Operation cancelled');
    }
    if (!entry.available) {
      throw new Error('Browser storage is unavailable. Enable storage to prepare offline use.');
    }

    if (!entry.stored) {
      if (entry.shell) {
        await prepareShell(signal);
      } else {
        await getResource(entry.asset, entry.url, signal, {
          persistent: true,
          onProgress: (loaded, phase) => onProgress({ loaded: completed + loaded, total, phase })
        });
      }
    }
    completed += entry.asset.size;
    onProgress({ loaded: completed, total, phase: 'save' });
  }

  return offlineState(signal);
}
