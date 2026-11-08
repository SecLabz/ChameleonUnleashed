import { sha256 } from './sha256.mjs';
import CONFIG from '../../config.json' with { type: 'json' };
import TABLE from './tables.json' with { type: 'json' };

export { TABLE };

export const RESOURCE_BASE =
  globalThis.document?.querySelector('meta[name="chameleon-resources"]')?.getAttribute('content') ||
  CONFIG.baseUrl;

export const TABLE_URL = resourceUrl('tables/' + TABLE.sha256 + '/' + TABLE.path);

/** @param {string} path */
export function resourceUrl(path) {
  return RESOURCE_BASE + path;
}

const loadedResources = new Map();

async function resourceCache(key, bytesToStore, signal) {
  let storage;

  try {
    storage = globalThis.indexedDB;
  } catch {
    return { available: false, value: null };
  }

  if (!storage) {
    return { available: false, value: null };
  }

  return new Promise((resolve) => {
    let database;
    let finished = false;

    const finish = (value, available = false) => {
      if (finished) {
        return;
      }

      finished = true;
      clearTimeout(timeout);
      signal?.removeEventListener('abort', stop);
      database?.close();
      resolve({ available, value });
    };

    const stop = () => finish(null);
    const timeout = setTimeout(stop, 1000);
    signal?.addEventListener('abort', stop, { once: true });

    if (signal?.aborted) {
      stop();
      return;
    }

    let opening;

    try {
      opening = storage.open('chameleon-resources', 1);
    } catch {
      stop();
      return;
    }

    opening.onupgradeneeded = () => opening.result.createObjectStore('tables');
    opening.onerror = stop;
    opening.onblocked = stop;
    opening.onsuccess = () => {
      database = opening.result;

      if (finished) {
        database.close();
        return;
      }

      try {
        const mode = bytesToStore ? 'readwrite' : 'readonly';
        const transaction = database.transaction('tables', mode);
        const store = transaction.objectStore('tables');
        const request = bytesToStore ? store.put(bytesToStore, key) : store.get(key);
        let value = null;

        request.onsuccess = () => {
          value = request.result;
        };

        transaction.oncomplete = () => finish(value, true);
        transaction.onerror = stop;
        transaction.onabort = stop;
      } catch {
        stop();
      }
    };
  });
}

async function validateResource(buffer, asset, signal) {
  if (
    !(buffer instanceof ArrayBuffer) ||
    buffer.byteLength !== asset.size ||
    (await sha256(new Uint8Array(buffer), signal)) !== asset.sha256
  ) {
    throw new Error('Resource has an unexpected size or SHA-256');
  }

  return buffer;
}

export async function resourceState(asset, signal) {
  const cached = await resourceCache(asset.sha256, undefined, signal);
  let stored = false;

  if (cached.value) {
    try {
      await validateResource(cached.value, asset, signal);
      stored = true;
    } catch {
      // An outdated or corrupt entry must be downloaded again before offline use.
    }
  }

  return { stored, available: cached.available, session: loadedResources.has(asset.sha256) };
}

async function saveResource(asset, buffer, signal, verify) {
  await resourceCache(asset.sha256, buffer, signal);

  if (signal?.aborted) {
    throw new Error('Operation cancelled');
  }

  if (verify && !(await resourceState(asset, signal)).stored) {
    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }
    throw new Error('Browser storage could not save this resource for offline use.');
  }
}

export async function getResource(asset, url, signal, { persistent = false, onProgress = () => {} } = {}) {
  if (signal?.aborted) {
    throw new Error('Operation cancelled');
  }

  if (loadedResources.has(asset.sha256)) {
    const bytes = loadedResources.get(asset.sha256);
    onProgress(asset.size, 'verify');
    if (persistent) {
      onProgress(asset.size, 'save');
      await saveResource(asset, bytes, signal, true);
    }
    return bytes;
  }

  const cached = await resourceCache(asset.sha256, undefined, signal);

  if (signal?.aborted) {
    throw new Error('Operation cancelled');
  }

  if (cached.value) {
    try {
      onProgress(asset.size, 'verify');
      return await validateResource(cached.value, asset, signal);
    } catch {
      // Fetch a valid replacement from the CDN.
    }
  }

  const controller = new AbortController();
  const stop = () => controller.abort();
  signal?.addEventListener('abort', stop, { once: true });

  if (signal?.aborted) {
    controller.abort();
  }

  const timeout = setTimeout(stop, 10000);

  try {
    onProgress(0, 'download');
    const response = await fetch(url, { mode: 'cors', signal: controller.signal });
    const contentLength = Number(response.headers.get('Content-Length'));

    if (!response.ok || contentLength > asset.size) {
      await response.body?.cancel();
      throw new Error('Asset server unavailable');
    }

    const reader = response.body.getReader();
    const bytes = new Uint8Array(asset.size);
    let offset = 0;

    for (;;) {
      const { done, value } = await reader.read();

      if (done) {
        break;
      }

      if (offset + value.length > bytes.length) {
        await reader.cancel();
        throw new Error('Oversized resource');
      }

      bytes.set(value, offset);
      offset += value.length;
      onProgress(offset, 'download');
    }

    if (offset !== asset.size) {
      throw new Error('Incomplete resource');
    }

    onProgress(asset.size, 'verify');
    await validateResource(bytes.buffer, asset, signal);

    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }

    loadedResources.set(asset.sha256, bytes.buffer);
    onProgress(asset.size, 'save');
    await saveResource(asset, bytes.buffer, signal, persistent);

    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }

    return bytes.buffer;
  } finally {
    clearTimeout(timeout);
    signal?.removeEventListener('abort', stop);
  }
}

export async function getTables(signal) {
  try {
    return await getResource(TABLE, TABLE_URL, signal);
  } catch (error) {
    if (signal?.aborted) {
      throw new Error('Operation cancelled');
    }

    throw new Error('Hardnested tables unavailable. Connect to the Internet and try again.', {
      cause: error
    });
  }
}
