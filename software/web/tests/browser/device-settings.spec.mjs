import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';

test.beforeEach(async ({ page }) => {
  const frontend = process.env.CHAMELEON_WEB_TEST_FRONTEND_APP;
  if (frontend) {
    const body = await readFile(frontend);
    await page.route('**/app.mjs', (route) => route.fulfill({ body, contentType: 'text/javascript' }));
  }
});

test('a setting save invalidates an older pending device refresh', async ({ page, request }) => {
  await request.post('/test/reset');
  await page.goto('/#device');
  const animation = page.locator('[data-setting="animation"]');
  await expect(animation).toHaveValue('0');
  let release;
  let first = true;
  let observed;
  const waiting = new Promise((resolve) => {
    observed = resolve;
  });
  await page.route('**/api/device', async (route) => {
    const response = await route.fetch();
    if (first) {
      first = false;
      observed();
      await new Promise((resolve) => {
        release = resolve;
      });
    }
    await route.fulfill({ response });
  });
  await page.evaluate(() => {
    globalThis.settingsRenders = [];
    new MutationObserver(() => {
      globalThis.settingsRenders.push(document.querySelector('[data-setting="animation"]').value);
    }).observe(document.querySelector('#device-settings'), { childList: true });
  });
  try {
    await page.locator('#refresh-device').click();
    await waiting;
    await animation.selectOption('1');
    await expect(page.locator('#toast')).toContainText('Saved on the device.');
  } finally {
    release?.();
  }
  await expect.poll(() => page.evaluate(() => globalThis.settingsRenders.length)).toBeGreaterThan(0);
  await expect(animation).toHaveValue('1');
  await expect(animation).toBeEnabled();
  expect((await (await request.get('/api/device')).json()).settings.animation).toBe(1);
});

test('settings stay locked through a concurrent refresh and duplicate change events', async ({
  page,
  request
}) => {
  await request.post('/test/reset');
  await page.goto('/#device');
  const longPress = page.locator('[data-setting="long_ms"]');
  const animation = page.locator('[data-setting="animation"]');
  await expect(longPress).toHaveValue('1000');
  let release;
  let observed;
  const waiting = new Promise((resolve) => {
    observed = resolve;
  });
  let writes = 0;
  await page.route('**/api/settings', async (route) => {
    writes++;
    observed();
    await new Promise((resolve) => {
      release = resolve;
    });
    await route.continue();
  });
  try {
    await longPress.fill('1001');
    await longPress.dispatchEvent('change');
    await waiting;
    await expect(animation).toBeDisabled();
    await expect(longPress).toBeDisabled();
    await longPress.dispatchEvent('change');
    await page.locator('#refresh-device').click();
    await expect(longPress).toHaveValue('1000');
    await expect(animation).toBeDisabled();
    await expect(longPress).toBeDisabled();
  } finally {
    release?.();
  }
  await expect(longPress).toHaveValue('1001');
  await expect(longPress).toBeEnabled();
  await expect(animation).toBeEnabled();
  expect(writes).toBe(1);
  expect((await (await request.get('/api/device')).json()).settings.long_ms).toBe(1001);
});
