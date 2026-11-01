import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';

let errors;

test.beforeEach(async ({ page, request }) => {
  await request.post('/test/reset');
  errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route(RESOURCE_BASE + '**', (route) => route.abort());
  await page.goto('/');
  await expect(page.locator('#scan')).toBeEnabled();
});

test.afterEach(() => expect(errors).toEqual([]));

async function newRfid(page) {
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  await page.locator('#create').click();
  await page.locator('[data-create="100"]').click();
  await expect(page.locator('#lf-card')).toBeVisible();
}

async function format(page, value, edited = false) {
  await page.locator('#lf-format').selectOption(String(value));
  if (edited) {
    await page.getByRole('button', { name: 'Change format', exact: true }).click();
  }
  await expect(page.locator('#lf-format')).toHaveValue(String(value));
  const name = await page.locator('#lf-format option:checked').textContent();
  await expect(page.locator('#studio-family')).toHaveText(name);
}

for (const width of [320, 360, 1280]) {
  test(`a new RFID document programs through one dialog and fits at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    const commands = [];
    page.on('request', (request) => {
      if (request.url().includes('/api/command/')) {
        commands.push({ url: request.url(), data: [...request.postDataBuffer()] });
      }
    });
    await newRfid(page);
    await expect(page.locator('#studio-family')).toHaveText('EM410x');
    await expect(page.locator('#lf-chip')).toContainText('Physical tag not checked');
    await expect(page.locator('#lf-id')).toBeFocused();
    await expect(page.locator('#copy-tag')).toBeDisabled();
    await page.locator('#lf-id').fill('0102xx');
    await expect(page.locator('#lf-error')).toBeVisible();
    await expect(page.locator('#export-json')).toBeDisabled();
    await page.locator('#lf-id').fill('01 02 03 04 05');
    await page.locator('#lf-id').press('Tab');
    await expect(page.locator('#lf-id')).toHaveValue('0102030405');
    expect(commands).toEqual([]);
    await page.screenshot({
      path: test.info().outputPath(`chameleon-rfid-create-${width}-${test.info().project.name}.png`),
      fullPage: true
    });
    await page.locator('#save-menu summary').click();
    await page.getByRole('button', { name: 'Program a tag…', exact: true }).click();
    const dialog = await page.locator('dialog[open]').elementHandle();
    await expect(page.getByRole('heading', { name: 'Program a tag', exact: true })).toBeVisible();
    expect(commands).toEqual([]);
    await page.getByRole('button', { name: 'Check target', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Program this target?' })).toBeVisible();
    expect(await dialog.evaluate((element) => element.isConnected && element.open)).toBe(true);
    const layout = await page.locator('dialog[open]').evaluate((element) => {
      const rect = element.getBoundingClientRect();
      return {
        left: rect.left,
        right: rect.right,
        viewport: innerWidth,
        overflow: element.scrollWidth > element.clientWidth,
        heights: [...element.querySelectorAll('button')].map(
          (button) => button.getBoundingClientRect().height
        )
      };
    });
    expect(layout.left).toBeGreaterThanOrEqual(0);
    expect(layout.right).toBeLessThanOrEqual(layout.viewport);
    expect(layout.overflow).toBe(false);
    expect(layout.heights.every((height) => height >= 40)).toBe(true);
    await page.screenshot({
      path: test.info().outputPath(`chameleon-rfid-program-${width}-${test.info().project.name}.png`),
      fullPage: true
    });
    await page.getByRole('button', { name: 'Program & verify', exact: true }).click();
    await expect(page.locator('#status')).toContainText('written and verified');
    await expect(page.locator('#toast')).toHaveText('RFID programmed and verified');
    await expect(page.locator('#toast')).toHaveClass('toast visible');
    await expect(page.locator('dialog[open]')).toHaveCount(0);
    expect(commands.map((command) => command.data[1])).toEqual([1, 2]);
    expect(commands[0].data.slice(-5)).toEqual([1, 2, 3, 4, 5]);
    await page.reload();
    await expect(page.locator('#lf-id')).toHaveValue('0102030405');
    await expect(page.locator('#lf-chip')).toContainText('Physical tag not checked');
  });
}

test('all RFID formats expose logical fields and preserve invalid text on reload', async ({ page }) => {
  await newRfid(page);
  await format(page, 200);
  await expect(page.locator('#lf-hid-format option')).toHaveCount(31);
  await page.locator('#lf-facility').fill('42');
  await page.locator('#lf-number').fill('65536');
  await expect(page.locator('#lf-number')).toHaveAttribute('aria-invalid', 'true');
  await page.reload();
  await expect(page.locator('#lf-facility')).toHaveValue('42');
  await expect(page.locator('#lf-number')).toHaveValue('65536');
  await expect(page.locator('#copy-tag')).toBeDisabled();
  await page.locator('#lf-hid-format').selectOption('26');
  await expect(page.locator('#lf-facility')).toHaveCount(0);
  await page.locator('#lf-number').fill('34359738367');
  await expect(page.locator('#copy-tag')).toBeEnabled();
  await format(page, 201, true);
  await page.locator('#lf-version').fill('1');
  await page.locator('#lf-facility').fill('255');
  await page.locator('#lf-number').fill('65535');
  await expect(page.locator('#copy-tag')).toBeEnabled();
  await format(page, 150, true);
  await page.locator('#lf-id').fill('Ab12Cd34');
  await page.locator('#lf-id').press('Tab');
  await expect(page.locator('#lf-id')).toHaveValue('Ab12Cd34');
  await format(page, 104, true);
  await page.locator('#lf-id').fill('0102030405');
  await expect(page.locator('#copy-tag')).toBeDisabled();
  await page.locator('#lf-epilogue').fill('1234567890abcdef');
  await expect(page.locator('#copy-tag')).toBeEnabled();
  await format(page, 310, true);
  await page.locator('#lf-id').fill('1234ABCD');
  await page.locator('#lf-record summary').click();
  await expect(page.locator('#lf-record-value')).toHaveText('4944544B1234ABCD');
  await format(page, 180, true);
  await page.locator('#lf-id').fill('0000000001');
  await expect(page.locator('#copy-tag')).toBeEnabled();
  await page.locator('#lf-format').selectOption('170');
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#lf-format')).toHaveValue('180');
  await expect(page.locator('#lf-id')).toHaveValue('0000000001');
  await format(page, 170, true);
  await page.locator('#lf-id').fill('00000001');
  await expect(page.locator('#copy-tag')).toBeEnabled();
});

test('RFID drafts can be created and exported without device or processing resources', async ({ page }) => {
  await page.route('**/api/device', (route) => route.fulfill({ status: 503 }));
  const workers = [];
  page.on('worker', (worker) => workers.push(worker.url()));
  await page.reload();
  await newRfid(page);
  await format(page, 170);
  await page.locator('#lf-id').fill('00000001');
  await expect(page.locator('#copy-tag')).toBeDisabled();
  await expect(page.locator('#export-json')).toBeEnabled();
  await page.reload();
  await expect(page.locator('#lf-id')).toHaveValue('00000001');
  await expect(page.locator('#export-menu')).toBeHidden();
  const downloading = page.waitForEvent('download');
  await page.getByRole('button', { name: 'Export JSON', exact: true }).click();
  const download = await downloading;
  expect(download.suggestedFilename()).toMatch(/\.json$/);
  const saved = JSON.parse(await readFile(await download.path(), 'utf8'));
  expect(saved.document.memory[0]).toEqual([0, 0, 0, 1]);
  await expect(page.locator('dialog[open]')).toHaveCount(0);
  expect(workers).toEqual([]);
});

test('cancelling a checked target releases its reservation and never programs', async ({ page }) => {
  await newRfid(page);
  await page.locator('#lf-id').fill('0102030405');
  let programs = 0;
  page.on('request', (request) => {
    if (request.url().includes('/api/command/3033') && request.postDataBuffer()[1] === 2) {
      programs++;
    }
  });
  await page.locator('#save-menu summary').click();
  await page.locator('#copy-tag').click();
  await page.getByRole('button', { name: 'Check target', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Program this target?' })).toBeVisible();
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#status')).toContainText('nothing written');
  await expect(page.locator('#copy-tag')).toBeEnabled();
  expect(programs).toBe(0);
  await page.locator('#save-menu summary').click();
  await page.locator('#copy-tag').click();
  await page.getByRole('button', { name: 'Check target', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Program this target?' })).toBeVisible();
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
});

for (const operation of [1, 2]) {
  test(`Stop during RFID operation ${operation} settles the bounded reply and releases the device`, async ({
    page,
    request
  }) => {
    await newRfid(page);
    await page.locator('#lf-id').fill('0102030405');
    let release;
    let reached;
    const pending = new Promise((resolve) => {
      reached = resolve;
    });
    await page.route('**/api/command/3033?*', async (route) => {
      if (route.request().postDataBuffer()[1] === operation) {
        await new Promise((resolve) => {
          release = resolve;
          reached();
        });
      }
      await route.continue();
    });
    await page.locator('#save-menu summary').click();
    await page.locator('#copy-tag').click();
    await page.getByRole('button', { name: 'Check target', exact: true }).click();
    if (operation === 2) {
      await page.getByRole('button', { name: 'Program & verify', exact: true }).click();
    }
    await pending;
    await page.locator('dialog[open]').getByRole('button', { name: 'Stop', exact: true }).click();
    release();
    await expect(page.locator('#status')).toContainText(
      operation === 1 ? 'nothing written' : 'written and verified'
    );
    await expect(page.locator('#copy-tag')).toBeEnabled();
    expect((await (await request.get('/api/device')).json()).busy).toBe(false);
  });
}

test('a newly created ioProx document saves and selects its RFID slot for emulation', async ({ page }) => {
  await newRfid(page);
  await format(page, 201);
  await page.locator('#lf-facility').fill('42');
  await page.locator('#lf-number').fill('12345');
  await page.locator('#save-menu summary').click();
  await page.locator('#emulate').click();
  await page.getByRole('button', { name: 'Slot 2 Empty place', exact: true }).click();
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('Slot 2 saved and selected for emulation');
  await expect(page.locator('#studio-target')).toContainText('Slot 2 · RFID');
  await expect(page.locator('#lf-version')).toHaveValue('1');
  await expect(page.locator('#lf-facility')).toHaveValue('42');
  await expect(page.locator('#lf-number')).toHaveValue('12345');
  await expect(page.locator('#emulate')).toHaveText('Emulating');
});
