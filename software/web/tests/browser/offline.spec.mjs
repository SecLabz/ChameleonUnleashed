import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { CORE } from '../../../../.cache/web/assets/modules/runtime/core-build.mjs';
import { HARDNESTED } from '../../../../.cache/web/assets/modules/runtime/hardnested-build.mjs';
import {
  RESOURCE_BASE,
  resourceUrl,
  TABLE,
  TABLE_URL
} from '../../../../.cache/web/assets/modules/runtime/assets.mjs';

const coreUrl = resourceUrl('core/' + CORE.path);
const scalarUrl = resourceUrl('hardnested/' + HARDNESTED.scalar.path);
const fixtures = new Map();
for (const [url, path] of [
  [coreUrl, 'core/' + CORE.path],
  [scalarUrl, 'hardnested/' + HARDNESTED.scalar.path],
  [resourceUrl('hardnested/' + HARDNESTED.simd.path), 'hardnested/' + HARDNESTED.simd.path],
  [TABLE_URL, 'tables/' + TABLE.sha256 + '/' + TABLE.path]
]) {
  fixtures.set(url, await readFile(new URL('../../../../.cache/web/cdn/' + path, import.meta.url)));
}

let errors;
let requests;

test.beforeEach(async ({ page, request }) => {
  await request.post('/test/reset');
  errors = [];
  requests = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route(RESOURCE_BASE + '**', (route) => {
    requests.push(route.request().url());
    const body = fixtures.get(route.request().url());
    if (!body) {
      return route.abort();
    }
    return route.fulfill({
      body,
      contentType: 'application/octet-stream',
      headers: { 'Access-Control-Allow-Origin': '*' }
    });
  });
});

test.afterEach(async ({ page }) => {
  expect(errors).toEqual([]);
  await expect(page.locator('#offline-resources')).not.toContainText(/Standard tools|Advanced recovery/);
});

async function ready(page) {
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  await expect(page.locator('#offline-prepare')).toBeHidden();
  await expect(page.locator('#offline-stop')).toBeHidden();
  await expect(page.locator('#offline-progress')).toBeHidden();
}

async function prepare(page) {
  await page.locator('#offline-prepare').click();
  await ready(page);
}

async function replaceCached(page, key, length) {
  await page.evaluate(
    async ({ key, length }) => {
      const database = await new Promise((resolve, reject) => {
        const opening = indexedDB.open('chameleon-resources', 1);
        opening.onsuccess = () => resolve(opening.result);
        opening.onerror = () => reject(opening.error);
      });
      await new Promise((resolve, reject) => {
        const transaction = database.transaction('tables', 'readwrite');
        const store = transaction.objectStore('tables');
        if (key) {
          store.put(new ArrayBuffer(length), key);
        } else {
          store.clear();
        }
        transaction.oncomplete = resolve;
        transaction.onerror = () => reject(transaction.error);
      });
      database.close();
    },
    { key, length }
  );
}

test('startup prepares resources without opening Device and works after an offline reload', async ({
  page
}) => {
  await page.goto('/');
  await expect(page.locator('#scan')).toBeEnabled();
  await ready(page);
  await page.getByRole('button', { name: 'Device', exact: true }).click();
  await ready(page);

  expect(requests).toEqual(expect.arrayContaining([coreUrl, scalarUrl, TABLE_URL]));
  expect(new Set(requests).size).toBe(requests.length);
  await page.unroute(RESOURCE_BASE + '**');
  let networkRequests = 0;
  await page.route(RESOURCE_BASE + '**', (route) => {
    networkRequests++;
    return route.abort();
  });
  await page.reload();

  await ready(page);

  await page.getByRole('button', { name: 'Scan', exact: true }).click();
  await page.locator('#scan').click();
  await expect(page.locator('#status')).toHaveText('Tag identified · Memory not read');
  await expect(page.locator('#read')).toBeEnabled();
  await page.locator('#read').click();
  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
  await expect(page.locator('#stop')).toBeHidden();
  expect(networkRequests).toBe(0);
});

test('cancellation retains completed files and retry downloads only missing resources', async ({ page }) => {
  let release;
  let started;
  const pending = new Promise((resolve) => {
    release = resolve;
  });
  const waiting = new Promise((resolve) => {
    started = resolve;
  });
  await page.route(
    scalarUrl,
    async (route) => {
      started();
      await pending;
      await route.abort().catch(() => {});
    },
    { times: 1 }
  );
  await page.goto('/#device');
  await waiting;
  await expect(page.locator('#offline-progress')).toBeVisible();
  await expect(page.locator('#offline-progress-label')).toHaveText('Downloading');
  await expect(page.locator('#offline-prepare')).toBeHidden();
  await page.locator('#offline-stop').click();
  release();

  await expect(page.locator('#offline-progress')).toBeHidden();
  await expect(page.locator('#offline-state')).toHaveText('Partially ready');
  await expect(page.locator('#offline-message')).toContainText('Resources already saved are kept');
  const coreRequests = requests.filter((url) => url === coreUrl).length;
  await prepare(page);

  expect(requests.filter((url) => url === coreUrl)).toHaveLength(coreRequests);
});

for (const navigation of ['reload', 'history restoration']) {
  test(`${navigation} cancels background downloads and resumes missing resources`, async ({ page }) => {
    await page.addInitScript((tableUrl) => {
      const fetch = window.fetch.bind(window);
      window.fetch = (url, options) => {
        if (url === tableUrl) {
          options?.signal?.addEventListener(
            'abort',
            () => {
              // Record cancellation before the old document disappears.
              const key = 'test-background-cancellations';
              localStorage.setItem(key, String(Number(localStorage.getItem(key) || 0) + 1));
            },
            { once: true }
          );
        }
        return fetch(url, options);
      };
    }, TABLE_URL);

    let release;
    let started;
    const pending = new Promise((resolve) => {
      release = resolve;
    });
    const waiting = new Promise((resolve) => {
      started = resolve;
    });
    await page.route(
      TABLE_URL,
      async (route) => {
        started();
        await pending;
        await route.abort().catch(() => {});
      },
      { times: 1 }
    );

    await page.goto('/');
    await waiting;
    const coreRequests = requests.filter((url) => url === coreUrl).length;
    if (navigation === 'reload') {
      await page.reload();
    } else {
      await page.evaluate(() => window.dispatchEvent(new PageTransitionEvent('pagehide')));
    }
    release();

    expect(await page.evaluate(() => localStorage.getItem('test-background-cancellations'))).toBe('1');
    if (navigation === 'history restoration') {
      await page.evaluate(() =>
        window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true }))
      );
    }
    await expect(page.locator('#offline-state')).toHaveText('Ready offline');
    expect(requests.filter((url) => url === coreUrl)).toHaveLength(coreRequests);
  });
}

test('startup repairs corrupt stored bytes and refresh notices a cleared cache', async ({ page }) => {
  await page.goto('/#device');
  await ready(page);
  const resources = [...requests];
  await replaceCached(page, TABLE.sha256, TABLE.size);
  requests.length = 0;
  await page.reload();

  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  expect(requests).toEqual([TABLE_URL]);
  await replaceCached(page, null, 0);
  await page.locator('#refresh-device').click();
  await expect(page.locator('#offline-state')).toHaveText('Session only');
  requests.length = 0;
  await prepare(page);
  expect(requests).toEqual(resources.filter((url) => url !== TABLE_URL));
});

test('unavailable browser storage cannot be advertised as ready', async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(window, 'indexedDB', {
      get() {
        throw new DOMException('Storage disabled for this test', 'SecurityError');
      }
    });
  });
  await page.goto('/#device');

  await expect(page.locator('#offline-state')).toHaveText('Storage unavailable');
  await expect(page.locator('#offline-prepare')).toBeHidden();
  expect(requests).toEqual([]);
});

test('browsers without SIMD prepare the scalar recovery module without requesting SIMD', async ({ page }) => {
  await page.addInitScript(() => {
    WebAssembly.validate = () => false;
  });
  await page.goto('/#device');
  await ready(page);

  expect(requests).toEqual([coreUrl, scalarUrl, TABLE_URL]);
});

test('failed cache writes remain session-only and can be saved later without another download', async ({
  page
}) => {
  await page.addInitScript(() => {
    window.failResourceWrites = true;
    const transaction = IDBDatabase.prototype.transaction;
    IDBDatabase.prototype.transaction = function (...args) {
      if (window.failResourceWrites && args[1] === 'readwrite') {
        throw new DOMException('Storage full for this test', 'QuotaExceededError');
      }
      return transaction.apply(this, args);
    };
  });
  await page.goto('/#device');

  await expect(page.locator('#offline-state')).toHaveText('Session only');
  await expect(page.locator('#offline-message')).toContainText('Browser storage could not save');
  expect(requests).toEqual([coreUrl]);
  await page.evaluate(() => {
    window.failResourceWrites = false;
  });
  await prepare(page);

  expect(requests.filter((url) => url === coreUrl)).toHaveLength(1);
});

test('a CDN failure leaves a retryable state and preserves completed resources', async ({ page }) => {
  await page.route(scalarUrl, (route) => route.fulfill({ status: 503 }), { times: 1 });
  await page.goto('/#device');

  await expect(page.locator('#offline-state')).toHaveText('Partially ready');
  await expect(page.locator('#offline-progress')).toBeHidden();
  await expect(page.locator('#offline-prepare')).toBeEnabled();
  await expect(page.locator('#offline-message')).toContainText('unavailable');
  await page.evaluate(() => window.dispatchEvent(new Event('online')));
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  expect(requests.filter((url) => url === coreUrl)).toHaveLength(1);
});

test('a scan interrupts background preparation and missing resources resume afterwards', async ({ page }) => {
  let started;
  let release;
  const waiting = new Promise((resolve) => {
    started = resolve;
  });
  const pending = new Promise((resolve) => {
    release = resolve;
  });
  await page.route(
    scalarUrl,
    async (route) => {
      started();
      await pending;
      await route.abort().catch(() => {});
    },
    { times: 1 }
  );
  await page.goto('/');
  await waiting;
  await page.locator('#scan').click();
  await expect(page.locator('#status')).toHaveText('Tag identified · Memory not read');
  await expect(page.locator('#read')).toBeEnabled();
  await page.locator('#read').click();
  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
  release();

  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  expect(requests.filter((url) => url === coreUrl)).toHaveLength(1);
});
