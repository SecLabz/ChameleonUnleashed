// The build embeds this configuration before the service worker template.
const { version: SHELL_VERSION, files: SHELL_FILES } = globalThis.CHAMELEON_SHELL;
const PREFIX = `chameleon-shell:${self.registration.scope}:`;
const CACHE = PREFIX + SHELL_VERSION;
const url = (path) => new URL(path, self.registration.scope).href;
const files = new Map(SHELL_FILES.map((file) => [url(file.path), file]));

async function valid(response, file) {
  if (!response?.ok) {
    return false;
  }
  const bytes = await response.arrayBuffer();
  if (bytes.byteLength !== file.size) {
    return false;
  }
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  const hex = Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
  return hex === file.sha256;
}

async function saveShell() {
  const cache = await caches.open(CACHE);
  for (const [address, file] of files) {
    const response = await fetch(address, { cache: 'no-store' });
    if (!(await valid(response.clone(), file))) {
      throw new Error('Incomplete or mismatched offline app');
    }
    await cache.put(address, response);
  }
}

let repairing;
self.addEventListener('install', (event) => {
  event.waitUntil(
    (async () => {
      try {
        await saveShell();
      } catch (error) {
        await caches.delete(CACHE);
        throw error;
      }
      // A new version waits until old app windows close. Never replace code in
      // the middle of a device operation, and never cache /api or BLE responses.
    })()
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    (async () => {
      for (const name of await caches.keys()) {
        if (name.startsWith(PREFIX) && name !== CACHE) {
          await caches.delete(name);
        }
      }
    })()
  );
});

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') {
    return;
  }
  const target = new URL(event.request.url);
  if (
    target.href === self.registration.scope ||
    target.pathname === new URL('./', self.registration.scope).pathname
  ) {
    target.pathname += 'index.html';
  }
  target.search = '';
  target.hash = '';
  if (!files.has(target.href)) {
    return;
  }
  event.respondWith(
    (async () => {
      const cache = await caches.open(CACHE);
      const response = await cache.match(target.href);
      const file = files.get(target.href);
      if (await valid(response?.clone(), file)) {
        return response;
      }
      // Restore evicted files only if they match THIS shell version. A newer
      // deployment must never mix a fresh module with an old cached document.
      try {
        const fresh = await fetch(target.href, { cache: 'no-store' });
        if (await valid(fresh.clone(), file)) {
          await cache.put(target.href, fresh.clone());
          return fresh;
        }
      } catch {
        // Offline: report an incomplete shell instead of serving mixed code.
      }
      return new Response('Offline app incomplete. Reopen online and close old app windows.', {
        status: 503
      });
    })()
  );
});

self.addEventListener('message', (event) => {
  if (!['shell-state', 'shell-prepare'].includes(event.data?.type) || !event.ports[0]) {
    return;
  }
  event.waitUntil(
    (async () => {
      let stored = false;
      try {
        if (event.data.type === 'shell-prepare') {
          // Re-registering an unchanged worker does not reliably reinstall its
          // evicted cache while it still controls a Chromium window.
          if (!repairing) {
            repairing = saveShell().finally(() => {
              repairing = null;
            });
          }
          await repairing;
        }
        const cache = await caches.open(CACHE);
        stored = true;
        for (const [address, file] of files) {
          if (!(await valid(await cache.match(address), file))) {
            stored = false;
            break;
          }
        }
      } catch {
        stored = false;
      } finally {
        event.ports[0].postMessage({ version: SHELL_VERSION, stored });
      }
    })()
  );
});
