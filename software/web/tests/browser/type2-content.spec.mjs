import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';

const draftKey = 'chameleon-studio-v1';

test.beforeEach(async ({ page, request }) => {
  await request.post('/test/reset');
  await page.route(RESOURCE_BASE + '**', async (route) => {
    const path = route.request().url().slice(RESOURCE_BASE.length);
    await route.fulfill({
      body: await readFile(new URL('../../../../.cache/web/cdn/' + path, import.meta.url)),
      contentType: 'application/octet-stream',
      headers: { 'Access-Control-Allow-Origin': '*' }
    });
  });
  await page.goto('/#studio');
  await expect(page.locator('#create')).toBeEnabled();
});

test('Amiibo identity appears on the first scan and clears on a fresh non-Amiibo read', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'type2' });
  const pages = Array.from({ length: 135 }, () => [0, 0, 0, 0]);
  pages[0] = [4, 1, 2, 0x8f];
  pages[1] = [3, 4, 5, 6];
  pages[3] = [0xf1, 0x10, 0xff, 0xee];
  pages[4][0] = 0xa5;
  pages[21] = [1, 0, 0, 0];
  pages[22] = [0, 0x41, 0xa9, 2];
  pages[131][3] = 4;
  pages[132][0] = 0x5f;
  await page.route('**/api/command/2028?*', async (route) => {
    const body = route.request().postDataBuffer();
    let response;
    if (body[0] === 0) {
      response = [0, 4, 4, 2, 1, 0, 0x11, 3];
    } else if (body[0] === 5) {
      response = pages.slice(body[1], body[10] + 1).flat();
    } else {
      await route.continue();
      return;
    }
    await route.fulfill({ body: Buffer.from([0, 0, ...response]) });
  });
  await page.goto('/');
  await page.locator('#scan').click();
  await expect(page.locator('#status')).toHaveText('Type 2 read finished');
  await expect(page.locator('#amiibo-badge')).toBeVisible();
  await expect(page.locator('#amiibo-id')).toHaveText('010000000041A902');
  await expect(page.locator('#type2-content')).toBeHidden();

  pages[4][0] = 0;
  await page.locator('#read').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Read', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('Type 2 read finished');
  await expect(page.locator('#amiibo-badge')).toBeHidden();
  await expect(page.locator('#amiibo-identity')).toBeHidden();
  await expect(page.locator('#amiibo-id')).toBeEmpty();
  await expect(page.locator('#type2-content')).toBeVisible();
});

for (const width of [320, 1280]) {
  test(`NDEF edits remain local, survive reload and fit at ${width}px`, async ({ page }, testInfo) => {
    await page.setViewportSize({ width, height: 900 });
    await page.locator('#studio-new').click();
    await page.locator('[data-create="1100"]').click();
    const host = page.locator('#type2-content');
    await expect(host.getByRole('textbox', { name: 'NDEF content' })).toBeVisible();
    await expect.poll(() => page.evaluate((key) => localStorage.getItem(key), draftKey)).not.toBeNull();
    const original = await page.evaluate((key) => JSON.parse(localStorage.getItem(key)).document, draftKey);
    const posts = [];
    page.on('request', (request) => {
      if (request.method() === 'POST') {
        posts.push(request.url());
      }
    });
    await host.getByRole('combobox').selectOption('uri');
    await host.getByRole('textbox').fill('https://example.com/bonjour');
    await host.getByRole('button', { name: 'Apply to document' }).click();
    await expect(host.getByRole('textbox')).toHaveValue('https://example.com/bonjour');
    await expect
      .poll(() => page.evaluate((key) => JSON.parse(localStorage.getItem(key)).document.memory, draftKey))
      .not.toEqual(original.memory);
    const changed = await page.evaluate((key) => JSON.parse(localStorage.getItem(key)).document, draftKey);
    expect(changed.memory.slice(0, 4)).toEqual(original.memory.slice(0, 4));
    expect(changed.memory.slice(40)).toEqual(original.memory.slice(40));
    expect(posts).toEqual([]);
    await expect(page.locator('#write-changes')).toBeDisabled();
    await host.getByRole('textbox').fill('x'.repeat(300));
    await expect(host.getByRole('button')).toBeDisabled();
    await page.reload();
    await expect(host.getByRole('textbox')).toHaveValue('https://example.com/bonjour');
    const overflow = await host.evaluate(
      (element) =>
        element.getBoundingClientRect().right > innerWidth || element.scrollWidth > element.clientWidth
    );
    expect(overflow).toBe(false);
    await page.screenshot({ path: testInfo.outputPath('ndef.png'), fullPage: true });
  });
}

test('SLIX-L privacy password is masked and an authentication failure ends the operation', async ({
  page
}) => {
  await page.goto('/');
  await expect(page.locator('[data-view="scan"]').getByRole('button', { name: /SLIX-L/ })).toHaveCount(0);
  await page.getByRole('radio', { name: 'RFID 125 kHz' }).click();
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  await page.locator('#studio-new').click();
  await page.locator('#create-search').fill('SLIX-L');
  await page.locator('[data-create="1500"]').click();
  await expect(page.locator('#studio-family')).toHaveText('SLIX-L');
  const protectedRead = page.getByRole('button', { name: 'Read with privacy password…', exact: true });
  const input = page.getByRole('textbox', { name: 'SLIX-L privacy password' });
  for (const width of [320, 1280]) {
    await page.setViewportSize({ width, height: 900 });
    await page.locator('#read-menu summary').focus();
    await page.keyboard.press('Enter');
    await expect(protectedRead).toBeVisible();
    expect(await protectedRead.evaluate((button) => button.getBoundingClientRect().right <= innerWidth)).toBe(
      true
    );
    await expect(protectedRead).toBeEnabled();
    await page.screenshot({
      path: test.info().outputPath(`chameleon-slix-${width}-${test.info().project.name}.png`)
    });
    await protectedRead.focus();
    await page.keyboard.press('Enter');
    await expect(input).toHaveAttribute('type', 'password');
    if (width === 320) {
      await page.getByRole('button', { name: 'Cancel', exact: true }).click();
    }
  }
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const original = await memory.inputValue();
  await expect(page.getByRole('button', { name: 'Read tag', exact: true })).toBeDisabled();
  await input.fill('1122');
  await expect(page.getByRole('button', { name: 'Read tag', exact: true })).toBeDisabled();
  await input.fill('11223344');
  let attempts = 0;
  await page.route('**/api/command/2312?*', async (route) => {
    attempts++;
    expect([...route.request().postDataBuffer()]).toEqual([0x11, 0x22, 0x33, 0x44]);
    await route.fulfill({ body: Buffer.from([0, 6]) });
  });
  await page.getByRole('button', { name: 'Read tag', exact: true }).click();
  await expect(page.locator('#scan')).toBeEnabled();
  await expect(page.locator('#toast')).toHaveText('Authentication rejected');
  expect(attempts).toBe(1);
  await expect(memory).toHaveValue(original);
  const stored = await page.evaluate(() => JSON.stringify({ ...localStorage }));
  expect(stored).not.toContain('11223344');
});
