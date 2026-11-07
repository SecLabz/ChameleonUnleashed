import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';

test.use({ hasTouch: true });

function withImportDefaults(document) {
  // Import materializes these optional empty fields; all other document
  // fields, including memory, keys and emulator data, must remain identical.
  return {
    ...document,
    card: { ...document.card, ats: document.card.ats ?? [] },
    warnings: document.warnings ?? []
  };
}

async function exportDocument(page, id) {
  const menu = page.locator('#export-menu');
  if (!(await menu.locator(id).isVisible())) {
    await menu.locator('summary').tap();
  }
  const downloading = page.waitForEvent('download');
  await page.locator(id).tap();
  const download = await downloading;
  expect(await download.failure()).toBeNull();
  return { name: download.suggestedFilename(), buffer: await readFile(await download.path()) };
}

async function importDocument(page, file) {
  await page.getByRole('button', { name: 'Close document', exact: true }).tap();
  const closing = page.getByRole('dialog', { name: 'Close this document?', exact: true });
  const empty = page.locator('#studio-empty');
  await expect(closing.or(page.locator('#studio-empty:visible')).first()).toBeVisible();
  if (await closing.isVisible()) {
    await closing.getByRole('button', { name: 'Close document', exact: true }).tap();
  }
  await expect(empty).toBeVisible();
  const choosing = page.waitForEvent('filechooser');
  await page.locator('#import').tap();
  await (await choosing).setFiles({ ...file, mimeType: 'text/plain' });
  await expect(page.locator('#studio-document')).toBeVisible();
}

for (const [type, label, units, lastUnit] of [
  [1002, 'Classic 2K', 128, 126],
  [1003, 'Classic 4K', 256, 254],
  [1102, 'NTAG216', 231, 223]
]) {
  test(`${label} exports and reimports its last user data offline on touch browsers`, async ({
    page,
    context
  }) => {
    const errors = [];
    const api = [];
    const observe = (tab) => {
      tab.on('pageerror', (error) => errors.push(error.message));
      tab.on('request', (request) => {
        if (new URL(request.url()).pathname.includes('/api/')) {
          api.push(request.url());
        }
      });
    };
    observe(page);
    await page.goto('./#device');
    await expect(page.locator('#offline-state')).toHaveText('Ready offline');
    await page.getByRole('button', { name: 'Studio', exact: true }).tap();
    await page.locator('#create').tap();
    await page.locator('#create-all summary').tap();
    await page.locator(`[data-create="${type}"]`).tap();
    await expect(page.locator('#studio-family')).toHaveText(label);

    const memory = page.locator('#memory textarea');
    const lines = (await memory.inputValue()).split('\n');
    expect(lines).toHaveLength(units);
    lines[lastUnit] = type < 1100 ? '0123456789ABCDEFFEDCBA9876543210' : 'A1B2C3D4';
    await memory.fill(lines.join('\n'));
    await expect(memory).toHaveAttribute('aria-invalid', 'false');
    const json = await exportDocument(page, '#export-document');
    const saved = JSON.parse(json.buffer.toString()).document;
    expect(saved.memory[lastUnit]).toEqual([...Buffer.from(lines[lastUnit], 'hex')]);
    const nfc = await exportDocument(page, '#export');
    expect(nfc.name).toMatch(/\.nfc$/);

    // Reopening offline tests the hosted shell and persisted WASM/resources,
    // not a routed mock of the USB app. No physical reader is available here.
    await page.close();
    await context.setOffline(true);
    const reopened = await context.newPage();
    observe(reopened);
    await reopened.goto('./#studio');
    await expect(reopened.locator('#studio-family')).toHaveText(label);
    // The editor groups hexadecimal bytes on reopening; compare data, not spaces.
    await expect
      .poll(async () => (await reopened.locator('#memory textarea').inputValue()).replace(/\s/g, ''))
      .toBe(lines.join('').replace(/\s/g, ''));
    await importDocument(reopened, nfc);
    const reimported = JSON.parse(
      (await exportDocument(reopened, '#export-document')).buffer.toString()
    ).document;
    expect(reimported.type).toBe(type);
    expect(reimported.card.uid).toEqual(saved.card.uid);
    expect(reimported.memory).toEqual(saved.memory);
    await importDocument(reopened, json);
    expect(
      withImportDefaults(
        JSON.parse((await exportDocument(reopened, '#export-document')).buffer.toString()).document
      )
    ).toEqual(withImportDefaults(saved));
    const layout = await reopened.evaluate(() => ({
      width: document.documentElement.scrollWidth,
      viewport: innerWidth
    }));
    expect(layout.width).toBeLessThanOrEqual(layout.viewport);
    await reopened.screenshot({ path: test.info().outputPath('offline-document.png') });
    expect(errors).toEqual([]);
    expect(api).toEqual([]);
  });
}
