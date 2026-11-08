import { test, expect } from '@playwright/test';

const PARTS = ['#connection-title', '#connection-help', '#connection-tools', '#device-rows'];
const measure = (page) => Promise.all(PARTS.map((selector) => page.locator(selector).boundingBox()));

test('connection controls sit beside the status they change', async ({ page }) => {
  await page.setViewportSize({ width: 1100, height: 800 });
  await page.goto('/#device');
  await expect(page.locator('#connection-help')).toHaveText('Connected over USB');
  await expect(page.locator('#emulation-resume')).toBeHidden();
  const bluetooth = await page.evaluate(() => !!navigator.bluetooth && isSecureContext);
  test.skip(!bluetooth, 'Only a browser with Web Bluetooth offers a second transport');
  const connect = page.locator('#connection-tools').getByRole('button', { name: 'Connect Bluetooth' });
  await expect(connect).toBeVisible();
  await expect(page.locator('#connection-actions')).toBeHidden();

  // The controls end the status row, in line with the values below.
  let [title, help, tools, rows] = await measure(page);
  expect(tools.y).toBeLessThan(help.y + help.height);
  expect(tools.y + tools.height).toBeGreaterThan(title.y);
  expect(tools.x + tools.width).toBeCloseTo(rows.x + rows.width, 0);

  // A narrow phone keeps the status readable and moves the controls below it.
  const line = title.height;
  await page.setViewportSize({ width: 320, height: 700 });
  [title, help, tools, rows] = await measure(page);
  expect(title.height).toBe(line);
  expect(tools.y).toBeGreaterThanOrEqual(help.y + help.height);
  expect(tools.x + tools.width).toBeCloseTo(rows.x + rows.width, 0);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
