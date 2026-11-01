import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';

const draftKey = 'chameleon-studio-v1';
const modelKey = 'chameleon-create-models-v1';

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

async function create(page, type) {
  await page.locator('#studio-new').click();
  const model = page.locator(`[data-create="${type}"]`);
  if (!(await model.count())) {
    await page.locator('#create-all summary').click();
  }
  await model.click();
  await expect(page.locator('#create-dialog')).not.toBeVisible();
}

async function draft(page) {
  await expect.poll(() => page.evaluate((key) => localStorage.getItem(key), draftKey)).not.toBeNull();
  return page.evaluate((key) => JSON.parse(localStorage.getItem(key)).document, draftKey);
}

function withCrc(data) {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0x8408 : 0);
    }
  }
  crc ^= 0xffff;
  return [0, 0, ...data, crc & 255, crc >>> 8];
}

for (const [type, name, blocks] of [
  [1500, 'SLIX-L', 8],
  [1501, 'SLIX', 28]
]) {
  test(`${name} opens from New and its first read adopts the tag after reload`, async ({ page }) => {
    const commands = [];
    page.on('request', (request) => {
      if (request.method() === 'POST' && request.url().includes('/api/command/')) {
        commands.push(request.url());
      }
    });
    await create(page, type);
    await expect(page.locator('#studio-family')).toHaveText(name);
    await expect(page.locator('#memory-status')).toHaveText(`0/${blocks} blocks known`);
    expect(commands).toEqual([]);
    const original = await draft(page);
    expect(original.card.uid).toHaveLength(8);
    expect(original.unbound).toBe(true);
    expect(original.emulator).toBeUndefined();
    await page.reload();
    await expect(page.locator('#studio-family')).toHaveText(name);
    const uid = [0xe0, 4, type === 1500 ? 3 : 1, 0x10, 2, 3, 4, 5];
    const password = type === 1500 ? [0x11, 0x22, 0x33, 0x44] : [];
    const requests = [];
    await page.route('**/api/command/*', async (route) => {
      const command = Number(new URL(route.request().url()).pathname.split('/').at(-1));
      const payload = [...(route.request().postDataBuffer() || [])];
      requests.push(command);
      let reply;
      if (command === (type === 1500 ? 2312 : 2300)) {
        expect(payload).toEqual(password);
        reply = withCrc([0, 12, ...[...uid].reverse()]);
      } else if (command === 2301) {
        expect(payload).toEqual([...uid, ...password]);
        reply = withCrc([0, 4, ...[...uid].reverse(), blocks - 1, 3]);
      } else if (command === 2308) {
        expect(payload).toEqual([...uid, 0, blocks, 4, ...password]);
        reply = new Uint8Array(2 + 24 + blocks * 6);
        reply.set([0, 0, 1, 3, 0, 0, ...uid, 0, blocks, 4]);
        for (let index = 0; index < blocks; index++) {
          reply.set([1, 1, 1, 2, 3, index], 26 + index * 6);
        }
      } else {
        throw new Error(`Unexpected NFC command ${command}`);
      }
      await route.fulfill({ body: Buffer.from(reply) });
    });
    if (type === 1500) {
      await page.locator('#read-menu summary').click();
      await page.locator('#read-slix').click();
      await page.getByRole('textbox', { name: 'SLIX-L privacy password' }).fill('11223344');
      await page.getByRole('button', { name: 'Read tag', exact: true }).click();
    } else {
      await expect(page.locator('#read-menu')).toBeHidden();
      await page.locator('#read').click();
    }
    await expect(page.locator('#status')).toHaveText('Read complete');
    await expect(page.locator('#card')).toContainText('E004');
    await expect(page.locator('#card')).toContainText('1002030405');
    await expect(page.locator('#memory-status')).toHaveText(`${blocks}/${blocks} blocks known`);
    await expect.poll(async () => (await draft(page)).card.uid).toEqual(uid);
    const saved = await draft(page);
    expect(saved.unbound).toBeUndefined();
    expect(saved.memory[0]).toEqual([1, 2, 3, 0]);
    expect(requests).toEqual([type === 1500 ? 2312 : 2300, 2301, 2308]);
    expect(JSON.stringify(saved)).not.toContain('11223344');
    await page.reload();
    await expect(page.locator('#memory-status')).toHaveText(`${blocks}/${blocks} blocks known`);
    expect((await draft(page)).unbound).toBeUndefined();
  });
}

for (const width of [320, 360, 1280]) {
  test(`creation opens four shortcuts in two clicks and fits at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    const commands = [];
    page.on('request', (request) => {
      if (request.method() === 'POST' && request.url().includes('/api/')) {
        commands.push(request.url());
      }
    });
    await page.locator('#create').click();
    await expect(page.locator('[data-create]')).toHaveCount(4);
    await expect(page.locator('#create-all')).not.toHaveAttribute('open', '');
    const layout = await page.locator('#create-dialog').evaluate((element) => {
      const rect = element.getBoundingClientRect();
      return {
        left: rect.left,
        right: rect.right,
        viewport: innerWidth,
        height: rect.height,
        screenHeight: innerHeight,
        overflow: element.scrollWidth > element.clientWidth,
        heights: [...element.querySelectorAll('[data-create]')].map(
          (button) => button.getBoundingClientRect().height
        )
      };
    });
    expect(layout.left).toBeGreaterThanOrEqual(0);
    expect(layout.right).toBeLessThanOrEqual(layout.viewport);
    expect(layout.overflow).toBe(false);
    expect(layout.height).toBeLessThan(layout.screenHeight * 0.65);
    expect(layout.heights.every((height) => height >= 44)).toBe(true);
    expect(await page.evaluate(() => document.activeElement.id)).not.toBe('create-search');
    await page.screenshot({
      path: test.info().outputPath(`chameleon-new-${width}-${test.info().project.name}.png`)
    });
    await page.locator('[data-create="1100"]').click();
    await expect(page.locator('#studio-family')).toHaveText('NTAG213');
    await expect(page.locator('#create-dialog')).not.toBeVisible();
    await expect(page.locator('#memory-status')).toHaveText('45/45 pages known');
    await expect(page.locator('#type2-defaults')).toBeHidden();
    await expect(page.locator('#status')).toContainText('nothing written');
    await expect(page.locator('#export')).toBeEnabled();
    await expect(page.locator('#save-slot')).toBeEnabled();
    const saved = await draft(page);
    expect(saved.card.uid).toHaveLength(7);
    expect(saved.card.uid[0]).toBe(4);
    expect(saved.memory[3]).toEqual([0xe1, 0x10, 0x12, 0]);
    expect(commands).toEqual([]);
    await page.reload();
    await expect(page.locator('#studio-family')).toHaveText('NTAG213');
    expect((await draft(page)).card.uid).toEqual(saved.card.uid);
  });
}

test('generated Classic UIDs cannot start with the cascade marker', async ({ page }) => {
  await page.evaluate(() => {
    crypto.getRandomValues = (bytes) => bytes.fill(0x88);
  });
  await create(page, 1001);
  const document = await draft(page);
  expect(document.card.uid).toHaveLength(4);
  expect(document.card.uid[0]).not.toBe(0x88);
  expect(document.memory[0].slice(0, 4)).toEqual(document.card.uid);
  expect(document.memory[0][4]).toBe(document.card.uid.reduce((bcc, byte) => bcc ^ byte, 0));
});

test('custom UIDs validate in place and a created Type 2 document exports and emulates', async ({ page }) => {
  await page.locator('#create').click();
  await page.locator('#create-identity summary').click();
  await page.locator('#create-uid').fill('01 02 03 04');
  await page.locator('[data-create="1101"]').click();
  await expect(page.locator('#create-error')).toContainText('exactly 7 bytes');
  await expect(page.locator('#studio-document')).toBeHidden();
  await page.locator('#create-uid').fill('04 01 02 03 04 05 06');
  await page.locator('[data-create="1101"]').click();
  await expect(page.locator('#studio-family')).toHaveText('NTAG215');
  await page.locator('#export-menu summary').click();
  const downloading = page.waitForEvent('download');
  await page.locator('#export').click();
  const text = await readFile(await (await downloading).path(), 'utf8');
  expect(text).toContain('UID: 04 01 02 03 04 05 06');
  expect(text).toContain('Pages total: 135');
  await page.locator('#save-menu summary').click();
  await page.locator('#emulate').click();
  await page.getByRole('button', { name: 'Slot 2 Empty place', exact: true }).click();
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('Slot 2 saved and selected for emulation');
});

test('opening, cancelling and declining replacement preserve edits; replacing keeps them in Recent', async ({
  page
}) => {
  await create(page, 100);
  await page.locator('#lf-id').fill('0102030405');
  await expect.poll(async () => (await draft(page)).memory[0]).toEqual([1, 2, 3, 4, 5]);
  const original = await draft(page);
  const history = await page.evaluate((key) => localStorage.getItem(key), modelKey);
  await page.locator('#studio-new').click();
  await expect(page.locator('#lf-id')).toHaveValue('0102030405');
  await page.getByRole('button', { name: 'Cancel creation', exact: true }).click();
  expect(await draft(page)).toEqual(original);
  await page.locator('#studio-new').click();
  await page.keyboard.press('Escape');
  await expect(page.locator('#create-dialog')).not.toBeVisible();
  expect(await draft(page)).toEqual(original);
  await page.locator('#studio-new').click();
  await page.locator('[data-create="1001"]').click();
  const confirmation = page.getByRole('dialog', { name: 'Replace the open document?', exact: true });
  await confirmation.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#create-dialog')).toBeVisible();
  expect(await page.evaluate((key) => localStorage.getItem(key), modelKey)).toBe(history);
  expect(await draft(page)).toEqual(original);
  await page.locator('[data-create="1001"]').click();
  await confirmation.getByRole('button', { name: 'Replace', exact: true }).click();
  await expect(page.locator('#studio-family')).toHaveText('Classic 1K');
  await page.getByRole('button', { name: 'Close document', exact: true }).click();
  await page.locator('#recents button').filter({ hasText: 'EM410x' }).click();
  await expect(page.locator('#lf-id')).toHaveValue('0102030405');
});

test('a failed creation keeps the edited document and allows retry in the same chooser', async ({ page }) => {
  await create(page, 100);
  await page.locator('#lf-id').fill('0102030405');
  await expect.poll(async () => (await draft(page)).memory[0]).toEqual([1, 2, 3, 4, 5]);
  const original = await draft(page);
  const history = await page.evaluate((key) => localStorage.getItem(key), modelKey);
  await page.route('**/worker.mjs', (route) => route.abort());
  await page.locator('#studio-new').click();
  await page.locator('[data-create="1100"]').click();
  await page.getByRole('button', { name: 'Replace', exact: true }).click();
  await expect(page.locator('#create-error')).toBeVisible();
  expect(await page.evaluate((key) => localStorage.getItem(key), modelKey)).toBe(history);
  await expect(page.locator('#lf-id')).toHaveValue('0102030405');
  expect(await draft(page)).toEqual(original);
  await page.unroute('**/worker.mjs');
  await page.locator('[data-create="1100"]').click();
  await page.getByRole('button', { name: 'Replace', exact: true }).click();
  await expect(page.locator('#studio-family')).toHaveText('NTAG213');
  await expect(page.locator('#create-dialog')).not.toBeVisible();
});

for (const action of ['button', 'Escape']) {
  test(`cancelling preparation with ${action} retains edits and releases the workspace`, async ({ page }) => {
    await create(page, 100);
    await page.locator('#lf-id').fill('0102030405');
    await expect.poll(async () => (await draft(page)).memory[0]).toEqual([1, 2, 3, 4, 5]);
    const original = await draft(page);
    let reached;
    let release;
    const requested = new Promise((resolve) => {
      reached = resolve;
    });
    const blocked = new Promise((resolve) => {
      release = resolve;
    });
    await page.route('**/worker.mjs', async (route) => {
      reached();
      await blocked;
      await route.continue();
    });
    try {
      await page.locator('#studio-new').click();
      await page.locator('[data-create="1100"]').click();
      await page.getByRole('button', { name: 'Replace', exact: true }).click();
      await requested;
      await expect(page.locator('#create-progress')).toBeVisible();
      if (action === 'Escape') {
        await page.keyboard.press('Escape');
      } else {
        await page.getByRole('button', { name: 'Cancel creation', exact: true }).click();
      }
      await expect(page.locator('#create-dialog')).not.toBeVisible();
      await expect(page.locator('#studio-new')).toBeEnabled();
      await expect(page.locator('#lf-id')).toHaveValue('0102030405');
      expect(await draft(page)).toEqual(original);
    } finally {
      release();
    }
    await page.unroute('**/worker.mjs');
    await page.locator('#studio-new').click();
    await expect(page.locator('#create-error')).toBeHidden();
    await page.locator('[data-create="1001"]').click();
    await page.getByRole('button', { name: 'Replace', exact: true }).click();
    await expect(page.locator('#studio-family')).toHaveText('Classic 1K');
  });
}

test('Studio import remains accessible with a document open and cancellation preserves it', async ({
  page
}) => {
  await create(page, 1001);
  const original = await draft(page);
  await expect(page.locator('#import')).toBeVisible();
  const choosing = page.waitForEvent('filechooser');
  await page.locator('#import').click();
  await choosing;
  await page.locator('#dump-file').evaluate((input) => input.dispatchEvent(new Event('cancel')));
  await expect(page.locator('#create-dialog')).not.toBeVisible();
  await expect(page.locator('#studio-family')).toHaveText('Classic 1K');
  expect(await draft(page)).toEqual(original);
});

test('All models expands the existing catalog without duplicate shortcuts', async ({ page }) => {
  await page.locator('#create').click();
  await page.locator('#create-all summary').click();
  await expect(page.locator('[data-create]')).toHaveCount(23);
  await expect(page.locator('#create-models h3')).toHaveText([
    'MIFARE Classic',
    'NTAG / Ultralight',
    'ICODE',
    'RFID (LF)'
  ]);
  await expect(page.locator('#create-suggestions')).toBeHidden();
  await page.locator('#create-all summary').click();
  await expect(page.locator('[data-create]')).toHaveCount(4);
  await page.locator('#create-all summary').click();
  await page.locator('[data-create="1104"]').click();
  await expect(page.locator('#studio-family')).toHaveText('Ultralight C');
});

test('search finds every model by family or spaced name and Enter creates the only match', async ({
  page
}) => {
  await page.locator('#create').click();
  await page.locator('#create-search').fill('nfc');
  await expect(page.locator('[data-create]')).toHaveCount(15);
  await expect(page.locator('#create-all')).toBeHidden();
  await page.locator('#create-search').fill('rfid');
  await expect(page.locator('[data-create]')).toHaveCount(8);
  await page.locator('#create-search').fill('unknown model');
  await expect(page.locator('[data-create]')).toHaveCount(0);
  await expect(page.locator('#create-none')).toBeVisible();
  await page.locator('#create-search').fill('');
  await expect(page.locator('[data-create]')).toHaveCount(4);
  await expect(page.locator('#create-none')).toBeHidden();
  await page.locator('#create-search').fill('  NTAG  216 ');
  await expect(page.locator('[data-create]')).toHaveCount(1);
  await expect(page.locator('[data-create="1102"]')).toBeVisible();
  await page.locator('#create-search').press('Enter');
  await expect(page.locator('#studio-family')).toHaveText('NTAG216');
});

test('an exact model name takes precedence over shared family and technology aliases', async ({ page }) => {
  await page.locator('#create').click();
  await page.locator('#create-search').fill('  UltraLight  C ');
  await expect(page.locator('[data-create]')).toHaveCount(1);
  await expect(page.locator('[data-create="1104"]')).toBeVisible();
  await page.locator('#create-search').press('Enter');
  await expect(page.locator('#create-dialog')).not.toBeVisible();
  await expect(page.locator('#studio-family')).toHaveText('Ultralight C');
});

test('the four last successful models become shortcuts and survive reload', async ({ page }) => {
  for (const type of [1000, 1102, 1104, 310]) {
    await create(page, type);
  }
  await page.reload();
  await page.locator('#studio-new').click();
  await expect(page.locator('[data-create]')).toHaveCount(4);
  expect(
    await page
      .locator('[data-create]')
      .evaluateAll((buttons) => buttons.map((button) => Number(button.dataset.create)))
  ).toEqual([310, 1104, 1102, 1000]);
  await page.locator('[data-create="1104"]').click();
  await expect(page.locator('#studio-family')).toHaveText('Ultralight C');
  await page.locator('#studio-new').click();
  expect(
    await page
      .locator('[data-create]')
      .evaluateAll((buttons) => buttons.map((button) => Number(button.dataset.create)))
  ).toEqual([1104, 310, 1102, 1000]);
});

test('unavailable storage keeps creation usable and reload restores default shortcuts', async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(window, 'localStorage', {
      get() {
        throw new DOMException('Storage disabled', 'SecurityError');
      }
    });
  });
  await page.reload();
  await create(page, 100);
  await expect(page.locator('#studio-family')).toHaveText('EM410x');
  await page.locator('#studio-new').click();
  expect(
    await page
      .locator('[data-create]')
      .evaluateAll((buttons) => buttons.map((button) => Number(button.dataset.create)))
  ).toEqual([100, 1001, 1100, 1101]);
  await page.reload();
  await page.locator('#create').click();
  expect(
    await page
      .locator('[data-create]')
      .evaluateAll((buttons) => buttons.map((button) => Number(button.dataset.create)))
  ).toEqual([1001, 1100, 1101, 100]);
});

test('unknown saved models are ignored and do not hide the catalog', async ({ page }) => {
  await page.evaluate((key) => localStorage.setItem(key, JSON.stringify([9999, '1100', null])), modelKey);
  await page.reload();
  await page.locator('#create').click();
  await expect(page.locator('[data-create]')).toHaveCount(4);
  await page.locator('#create-all summary').click();
  await expect(page.locator('[data-create]')).toHaveCount(23);
});
