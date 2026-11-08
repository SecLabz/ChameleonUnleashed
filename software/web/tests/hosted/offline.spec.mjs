import { test, expect } from '@playwright/test';

test('a prefixed hosted app reopens offline with its worker and saved document', async ({
  page,
  context
}) => {
  const errors = [];
  const apiRequests = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('request', (request) => {
    if (new URL(request.url()).pathname.includes('/api/')) {
      apiRequests.push(request.url());
    }
  });
  await page.goto('./#device');
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  await expect(page.locator('#use-usb')).toBeHidden();
  await expect(page.locator('#connect-bluetooth')).toHaveText('Connect Bluetooth');
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  await page.locator('#create').click();
  await page.locator('[data-create="1001"]').click();
  await expect(page.locator('#studio-family')).toHaveText('Classic 1K');
  await expect.poll(() => page.evaluate(() => localStorage.getItem('chameleon-studio-v1'))).not.toBeNull();
  const draft = await page.evaluate(() => localStorage.getItem('chameleon-studio-v1'));

  // A new page models reopening, not just a warm SPA navigation. The service
  // worker serves the shell; WASM and tables must come from persistent storage.
  await page.close();
  await context.setOffline(true);
  const reopened = await context.newPage();
  reopened.on('pageerror', (error) => errors.push(error.message));
  await reopened.goto('./#studio');
  await expect(reopened.locator('#studio-family')).toHaveText('Classic 1K');
  expect(await reopened.evaluate(() => localStorage.getItem('chameleon-studio-v1'))).toBe(draft);
  await reopened.getByRole('button', { name: 'Device', exact: true }).click();
  await expect(reopened.locator('#offline-state')).toHaveText('Ready offline');
  await reopened.getByRole('button', { name: 'Studio', exact: true }).click();
  await reopened.locator('#studio-new').click();
  await reopened.locator('[data-create="1100"]').click();
  await expect(reopened.locator('#studio-family')).toHaveText('NTAG213');
  expect(errors).toEqual([]);
  expect(apiRequests).toEqual([]);
});

test('evicting part of the interface removes Ready offline and Retry reinstalls it', async ({ page }) => {
  await page.goto('./#device');
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  await page.evaluate(async () => {
    const names = (await caches.keys()).filter((name) => name.startsWith('chameleon-shell:'));
    const cache = await caches.open(names[0]);
    await cache.delete(new URL('worker.mjs', document.baseURI).href);
  });
  await page.locator('#refresh-device').click();
  await expect(page.locator('#offline-state')).toHaveText('Partially ready');
  await page.locator('#offline-prepare').click();
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
});

test('an entirely evicted interface can reopen online without mixing shell versions', async ({ page }) => {
  await page.goto('./#device');
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  await page.reload();
  await page.evaluate(async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith('chameleon-shell:')) {
        await caches.delete(name);
      }
    }
  });
  await page.reload();
  await expect(page.locator('#offline-state')).toHaveText('Ready offline');
  await expect(page.locator('#connect-bluetooth')).toHaveText('Connect Bluetooth');
});
