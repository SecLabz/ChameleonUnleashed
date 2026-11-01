import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';
import { createEngine } from '../../../../.cache/web/assets/modules/runtime/nfc-wasm/engine.mjs';

const draftKey = 'chameleon-studio-v1';

let errors;

test.beforeEach(async ({ page, request }) => {
  await request.post('/test/reset');
  errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route(RESOURCE_BASE + '**', async (route) => {
    const path = route.request().url().slice(RESOURCE_BASE.length);
    const body = await readFile(new URL('../../../../.cache/web/cdn/' + path, import.meta.url));
    await route.fulfill({
      body,
      contentType: 'application/octet-stream',
      headers: { 'Access-Control-Allow-Origin': '*' }
    });
  });
  await page.goto('/');

  await expect(page.locator('#scan')).toBeEnabled();
});

test.afterEach(() => expect(errors).toEqual([]));

async function scan(page, read = true) {
  await page.locator('#scan').click();

  await expect(page.locator('#studio-document')).toBeVisible();
  await expect(page.locator('#stop')).toBeHidden();
  await expect(page.locator('#scan')).toBeEnabled();
  if (
    read &&
    (await page.locator('#identity-title').isVisible()) &&
    (await page.locator('#identity-title').textContent()) === 'Memory not read'
  ) {
    await page.locator('#read').click();
    await expect(page.locator('#stop')).toBeHidden();
    await expect(page.locator('#scan')).toBeEnabled();
  }
}

async function closeDocument(page) {
  await page.getByRole('button', { name: 'Close document', exact: true }).click();
}

async function importFile(page, name, text) {
  await page.locator('#dump-file').setInputFiles({ name, mimeType: 'text/plain', buffer: Buffer.from(text) });
}

async function exported(page, button) {
  let action = page.locator(button);
  if (!(await action.isVisible())) {
    await page.locator('#export-menu summary').click();
    if (button === '#export-json') {
      action = page.locator('#export-document');
    }
  }
  const download = page.waitForEvent('download');
  await action.click();
  const text = await readFile(await (await download).path(), 'utf8');
  await expect(page.locator('dialog[open]')).toHaveCount(0);

  return text;
}

async function keys(page) {
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Keys…', exact: true }).click();

  await expect(page.locator('#keys')).toContainText('FFFFFFFFFFFF');

  await page.locator('#keys-dialog button').click();
}

async function captureDetails(page) {
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Capture details…', exact: true }).click();
  return page.getByRole('dialog', { name: 'Capture details', exact: true });
}

test('scan, edit validation, NFC/JSON/MCT downloads and recent drafts use the real document', async ({
  page
}) => {
  await scan(page);

  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
  await expect(page.locator('#status')).toHaveText('Read complete');
  await expect(page.locator('#read-progress')).toBeHidden();

  await keys(page);

  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const original = await memory.inputValue();
  await memory.fill(original.slice(1));

  await expect(memory).toHaveAttribute('aria-invalid', 'true');

  for (const id of ['export', 'export-document', 'save-slot', 'emulate', 'copy-tag']) {
    await expect(page.locator('#' + id)).toBeDisabled();
  }
  await memory.fill(original.replace('00000000', '01020304'));

  await expect(memory).toHaveAttribute('aria-invalid', 'false');

  const json = await exported(page, '#export-json');

  expect(JSON.parse(json).document.memory[0].slice(8, 12)).toEqual([1, 2, 3, 4]);
  expect(await exported(page, '#export')).toContain('Filetype: Flipper NFC device');
  expect(await exported(page, '#export-mct')).toContain('+Sector: 0');

  await page.reload();

  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');

  await closeDocument(page);
  const closing = page.getByRole('dialog', { name: 'Close this document?', exact: true });
  await expect(closing).toContainText('This document will remain in Recent for this session.');
  await closing.getByRole('button', { name: 'Cancel', exact: true }).click();
  expect(await memory.inputValue()).toContain('01020304');
  await closeDocument(page);
  await closing.getByRole('button', { name: 'Close document', exact: true }).click();

  await expect(page.locator('#studio-empty')).toBeVisible();

  await page.locator('#recents button').first().click();

  expect(await memory.inputValue()).toContain('01020304');

  await closeDocument(page);
  await importFile(page, 'capture.json', json);

  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
});

for (const failure of ['uid', 'missing', 'cancelled']) {
  test(`failed Classic reread preserves edits and close confirmation: ${failure}`, async ({ page }) => {
    await scan(page);
    const identity = await page.locator('#card').innerText();
    const memory = page.locator('#memory textarea');
    const lines = (await memory.inputValue()).split('\n');
    lines[1] = '11'.repeat(16);
    const edited = lines.join('\n');
    await memory.fill(edited);
    let reached;
    let release;
    const started = new Promise((resolve) => {
      reached = resolve;
    });
    const pending = new Promise((resolve) => {
      release = resolve;
    });
    await page.route('**/api/command/2000?*', async (route) => {
      reached();
      if (failure === 'cancelled') {
        await pending;
        await route.continue();
      } else if (failure === 'missing') {
        await route.fulfill({ body: Buffer.from([0, 1]) });
      } else {
        const response = await route.fetch();
        const body = await response.body();
        body[3] = 9;
        await route.fulfill({ response, body });
      }
    });
    await page.locator('#read').click();
    await page.getByRole('button', { name: 'Recover', exact: true }).click();
    await started;
    if (failure === 'cancelled') {
      await page.locator('#stop').click();
      release();
    }
    await expect(page.locator('#stop')).toBeHidden();
    await expect(memory).toHaveValue(edited);
    expect(JSON.parse(await exported(page, '#export-json')).document.card.uid).toEqual([1, 2, 3, 4]);
    await closeDocument(page);
    const closing = page.getByRole('dialog', { name: 'Close this document?', exact: true });
    await expect(closing).toBeVisible();
    await closing.getByRole('button', { name: 'Cancel', exact: true }).click();
    await page.reload();
    await expect
      .poll(async () => (await memory.inputValue()).replace(/\s/g, ''))
      .toBe(edited.replace(/\s/g, ''));
    await expect(page.locator('#card')).toHaveText(identity);
    await closeDocument(page);
    await expect(closing).toBeVisible();
  });
}

test('Studio rename updates the document, confirms success and survives export and reload', async ({
  page
}) => {
  await scan(page);
  await page.getByRole('button', { name: 'Rename document', exact: true }).click();

  const dialog = page.getByRole('dialog', { name: 'Rename document', exact: true });
  await expect(dialog.getByRole('textbox')).toHaveValue('Tag-01020304');

  await dialog.getByRole('textbox').fill('  Badge entrée  ');
  await dialog.getByRole('button', { name: 'Rename', exact: true }).click();

  await expect(dialog).toHaveCount(0);
  await expect(page.locator('#studio-name')).toHaveText('Badge entrée');
  await expect(page.locator('#toast')).toHaveClass('toast visible');
  await expect(page.locator('#toast')).toHaveText('Document renamed to “Badge entrée”');
  expect(JSON.parse(await exported(page, '#export-json')).document.name).toBe('Badge entrée');
  await expect
    .poll(() => page.evaluate((key) => JSON.parse(localStorage.getItem(key)).document.name, draftKey))
    .toBe('Badge entrée');

  await page.reload();

  await expect(page.locator('#studio-name')).toHaveText('Badge entrée');
  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
});

test('Studio rename cancellation and invalid names preserve the current document', async ({ page }) => {
  await scan(page);
  await page.getByRole('button', { name: 'Rename document', exact: true }).click();

  let dialog = page.getByRole('dialog', { name: 'Rename document', exact: true });
  await dialog.getByRole('textbox').fill('Cancelled name');
  await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();

  await expect(page.locator('#studio-name')).toHaveText('Tag-01020304');
  await expect(page.locator('#toast')).not.toHaveClass(/visible/);

  await page.getByRole('button', { name: 'Rename document', exact: true }).click();
  dialog = page.getByRole('dialog', { name: 'Rename document', exact: true });
  await dialog.getByRole('textbox').fill('   ');
  await dialog.getByRole('button', { name: 'Rename', exact: true }).click();

  await expect(page.locator('#studio-name')).toHaveText('Tag-01020304');
  await expect(page.locator('#toast')).toHaveClass(/error/);
  await expect(page.locator('#toast')).toContainText('Use a name');
  expect(JSON.parse(await exported(page, '#export-json')).document.name).toBe('Tag-01020304');
});

test('slot strip selects the device slot without scrolling and stays in sync with card selection', async ({
  page,
  request
}) => {
  await page.getByRole('button', { name: 'Slots', exact: true }).click();
  const strip = page.locator('#slot-strip');
  const first = strip.getByRole('button', { name: /^Slot 1\b/ });
  const last = strip.getByRole('button', { name: /^Slot 8\b/ });
  await expect(first).toHaveAttribute('aria-pressed', 'true');
  await expect(last).toHaveAttribute('aria-pressed', 'false');
  const scrollY = await page.evaluate(() => window.scrollY);

  await last.click();

  await expect(last).toHaveAttribute('aria-pressed', 'true');
  await expect(last).toHaveClass('slot-led-btn active tags-off');
  await expect(first).toHaveAttribute('aria-pressed', 'false');
  await expect(page.locator('#slot-7 .slot-active')).toHaveText('Selected · NFC and RFID disabled');
  await expect(page.locator('#emulation-title')).toHaveText('Slot 8 selected');
  expect((await (await request.get('/api/device')).json()).active_slot).toBe(7);
  expect(await page.evaluate(() => window.scrollY)).toBe(scrollY);

  await page.locator('#slot-0').getByRole('button', { name: 'Select slot', exact: true }).click();

  await expect(first).toHaveAttribute('aria-pressed', 'true');
  await expect(last).toHaveAttribute('aria-pressed', 'false');
  await expect(page.locator('#slot-0 .slot-active')).toHaveText('Selected');

  await last.focus();
  await last.press('Enter');

  await expect(last).toHaveAttribute('aria-pressed', 'true');
  expect((await (await request.get('/api/device')).json()).busy).toBe(false);
});

test('slot refresh preserves keyboard focus when device state is unchanged', async ({ page }) => {
  await page.getByRole('button', { name: 'Slots', exact: true }).click();
  const last = page.locator('#slot-strip').getByRole('button', { name: /^Slot 8\b/ });
  await last.focus();

  await page.locator('#refresh-slots').evaluate((button) => button.click());

  await expect(page.locator('#toast')).toHaveText('Slots refreshed');
  await expect(last).toBeFocused();
});

for (const status of [409, 503]) {
  test(`slot strip reports activation failure and allows retry: ${status}`, async ({ page, request }) => {
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    const strip = page.locator('#slot-strip');
    const first = strip.getByRole('button', { name: /^Slot 1\b/ });
    const last = strip.getByRole('button', { name: /^Slot 8\b/ });
    await expect(last).toBeVisible();
    await page.route('**/api/slots/activate?*', (route) => route.fulfill({ status }), { times: 1 });

    await last.click();

    await expect(page.locator('#toast')).toHaveClass('toast visible error');
    await expect(first).toHaveAttribute('aria-pressed', 'true');
    await expect(last).toHaveAttribute('aria-pressed', 'false');
    expect((await (await request.get('/api/device')).json()).busy).toBe(false);

    await last.click();

    await expect(last).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator('#emulation-title')).toHaveText('Slot 8 selected');
    expect((await (await request.get('/api/device')).json()).busy).toBe(false);
  });
}

for (const edited of [false, true]) {
  test(`Studio rename persists the slot name and preserves memory edits: ${edited}`, async ({ page }) => {
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    await page.locator('#slot-0 .bay-open').first().click();

    await expect(page.locator('#studio-name')).toHaveText('Test Classic');
    await expect(page.locator('#rename')).toBeEnabled();

    const memory = page.locator('#memory textarea');
    const originalMemory = await memory.inputValue();
    let expectedMemory = originalMemory;
    if (edited) {
      expectedMemory = expectedMemory.replace('00000000', '01020304');
      await memory.fill(expectedMemory);
    }
    const writes = [];
    page.on('request', (request) => {
      if (/\/api\/slots\/(begin|write|commit)\?/.test(request.url())) {
        writes.push(request.url());
      }
    });
    await page.getByRole('button', { name: 'Rename document', exact: true }).click();

    const dialog = page.getByRole('dialog', { name: 'Rename document', exact: true });
    await dialog.getByRole('textbox').fill('Renamed slot');
    await dialog.getByRole('textbox').press('Enter');

    await expect(page.locator('#studio-name')).toHaveText('Renamed slot');
    await expect(page.locator('#toast')).toHaveText('Document renamed to “Renamed slot”');
    await expect(page.locator('#rename')).toBeEnabled();
    await expect(memory).toHaveValue(expectedMemory);
    if (edited) {
      await expect(page.locator('#studio-target')).toContainText('edited');
    } else {
      await expect(page.locator('#emulate')).toHaveText('Emulating');
    }
    expect(writes).toEqual([]);

    await page.getByRole('button', { name: 'Slots', exact: true }).click();

    await expect(page.locator('#slot-0 .bay-open').first()).toContainText('Renamed slot');

    await page.locator('#slot-0 .bay-open').first().click();

    await expect(memory).toHaveValue(expectedMemory);

    if (edited) {
      await memory.fill(originalMemory);

      await expect(page.locator('#emulate')).toHaveText('Emulating');
    }

    await page.getByRole('button', { name: 'Scan', exact: true }).click();
    await scan(page);
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    await page.locator('#slot-0 .bay-open').first().click();

    await expect(page.locator('#studio-name')).toHaveText('Renamed slot');
    await expect(memory).toHaveValue(originalMemory);
  });
}

test('Studio rename reports a failed slot update without changing the document name', async ({
  page,
  request
}) => {
  await page.getByRole('button', { name: 'Slots', exact: true }).click();
  await page.locator('#slot-0 .bay-open').first().click();

  await expect(page.locator('#rename')).toBeEnabled();

  await page.route('**/api/slots/nick?*', (route) => route.fulfill({ status: 503 }));
  await page.getByRole('button', { name: 'Rename document', exact: true }).click();

  const dialog = page.getByRole('dialog', { name: 'Rename document', exact: true });
  await dialog.getByRole('textbox').fill('Failed rename');
  await dialog.getByRole('button', { name: 'Rename', exact: true }).click();

  await expect(page.locator('#toast')).toHaveClass('toast visible error');
  await expect(page.locator('#studio-name')).toHaveText('Test Classic');
  await expect(page.locator('#rename')).toBeEnabled();
  expect((await (await request.get('/api/device')).json()).busy).toBe(false);

  await page.getByRole('button', { name: 'Slots', exact: true }).click();

  await expect(page.locator('#slot-0 .bay-open').first()).toContainText('Test Classic');
});

test('ST25TB scanning preserves blocks and metadata through export and reload', async ({ page, request }) => {
  await request.post('/test/reset', { data: 'st25tb' });

  await scan(page);

  await expect(page.locator('#memory-status')).toHaveText('16/16 blocks known');

  const saved = JSON.parse(await exported(page, '#export-json')).document;

  expect(saved.kind).toBe('st25tb');
  expect(saved.memory).toEqual(Array.from({ length: 16 }, (_, block) => [block, 1, 2, 3]));
  expect(saved.card.chip_id).toBe(0x5a);
  expect(saved.card.system).toEqual([255, 255, 255, 255]);

  await page.reload();

  await expect(page.locator('#memory-status')).toHaveText('16/16 blocks known');
  expect(JSON.parse(await exported(page, '#export-json')).document).toEqual(saved);

  await importFile(page, 'st25tb.nfc', await exported(page, '#export'));
  await expect(page.locator('#hf-defaults')).toBeVisible();
  await page.locator('#hf-defaults').click();
  await page
    .getByRole('dialog', { name: 'Choose emulator Chip ID' })
    .getByRole('button', { name: 'Use Chip ID', exact: true })
    .click();
  await expect(page.locator('#toast')).toHaveText('Emulator defaults applied');
  await expect(page.locator('#doc-notes')).toBeHidden();
  await expect(page.locator('#capture-details-dialog')).toBeHidden();
  await expect(page.locator('#hf-details')).not.toContainText('Chip ID was explicitly chosen');
  await expect(page.locator('#status')).toBeEmpty();

  let details = await captureDetails(page);
  await expect(details).toContainText('Emulator Chip ID was explicitly chosen');
  await details.getByRole('button', { name: 'Close', exact: true }).click();

  await page.reload();
  await expect(page.locator('#doc-notes')).toBeHidden();
  await expect(page.locator('#capture-details-dialog')).toBeHidden();
  details = await captureDetails(page);
  await expect(details).toContainText('Emulator Chip ID was explicitly chosen');
});

test('Stop before reading memory preserves recovered keys through reload and JSON export', async ({
  page
}) => {
  let release;
  let calls = 0;
  const waiting = new Promise((resolve) => {
    page.route('**/api/command/2015?*', async (route) => {
      calls++;
      if (calls === 2) {
        release = () => route.continue();
        resolve();
      } else {
        await route.continue();
      }
    });
  });
  await scan(page, false);
  await page.locator('#read').click();
  await waiting;
  await page.locator('#stop').click();
  await release();

  await expect(page.locator('#read')).toBeVisible();
  await expect
    .poll(() =>
      page.evaluate((key) => {
        const draft = JSON.parse(localStorage.getItem(key) || 'null');

        return !!draft?.document?.recoveredKeys?.some((keys) => keys.key_a);
      }, draftKey)
    )
    .toBe(true);

  await page.reload();

  await expect(page.locator('#memory-status')).toHaveText('0/64 blocks known');

  await keys(page);

  const saved = JSON.parse(await exported(page, '#export-json')).document;

  expect(saved.recoveredKeys.some((keys) => keys.key_a)).toBe(true);
  expect(saved.memory.every((block) => block === null)).toBe(true);
});

for (const technology of ['nfc', 'rfid']) {
  for (const dismissal of ['Cancel', 'Escape']) {
    test(`${technology} scan ${dismissal} closes immediately and retains the document while cleanup finishes`, async ({
      page,
      request
    }) => {
      await scan(page);
      await page.getByRole('button', { name: 'Scan', exact: true }).click();
      await page.locator(`[data-scan-mode=${technology}]`).click();
      let release;
      const pendingReply = new Promise((resolve) => {
        release = resolve;
      });
      await page.route('**/api/command/*', async (route) => {
        await pendingReply;
        await route.fulfill({ body: Buffer.from([0, technology === 'rfid' ? 0x41 : 1]) });
      });
      const command = page.waitForRequest('**/api/command/*');
      await page.locator('#scan').click();
      await command;

      const dialog = page.locator('#scan-dialog');
      await expect(dialog).toBeVisible();
      await expect(dialog.getByRole('button', { name: 'Cancel', exact: true })).toBeEnabled();
      await expect(dialog).not.toContainText(/Scanning ·/);

      if (dismissal === 'Escape') {
        await page.keyboard.press('Escape');
      } else {
        await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
      }

      await expect(dialog).toBeHidden();
      await expect(page.locator('#scan')).toBeDisabled();

      release();

      await expect(page.locator('#scan')).toBeEnabled();
      await expect(dialog).toBeHidden();
      expect((await (await request.get('/api/device')).json()).busy).toBe(false);

      await page.getByRole('button', { name: 'Studio', exact: true }).click();

      await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
      await expect(page.locator('#read-elapsed')).toBeHidden();

      await page.unroute('**/api/command/*');
      await page.getByRole('button', { name: 'Scan', exact: true }).click();
      await scan(page);
    });
  }
}

test('a failed scan can be dismissed with Cancel and retried', async ({ page }) => {
  await page.route('**/api/command/*', (route) => route.fulfill({ body: Buffer.from([0, 2]) }));
  await page.locator('#scan').click();

  const dialog = page.locator('#scan-dialog');
  await expect(dialog.getByRole('heading')).toHaveText('Scan unavailable');
  await expect(page.locator('#scan-result')).not.toBeEmpty();
  await expect(page.locator('#scan')).toBeEnabled();

  await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();

  await expect(dialog).toBeHidden();

  await page.unroute('**/api/command/*');
  await scan(page);

  await expect(page.locator('#read-elapsed')).toHaveText(/^\d+\.\d s$/);
});

test('RFID editing, no-op imports, dirty replacement and storage failures remain usable', async ({
  page
}) => {
  await page.locator('[data-scan-mode=rfid]').click();
  await scan(page);

  const json = await exported(page, '#export-json');
  await page.locator('#lf-id').fill('01');

  await expect(page.locator('#export-json')).toBeDisabled();

  await page.locator('#lf-id').fill('0102030406');

  await expect(page.locator('#export-json')).toBeEnabled();

  await importFile(page, 'same.json', json);

  await expect(page.locator('dialog[open]')).toHaveCount(0);
  await expect(page.locator('#lf-id')).toHaveValue('0102030406');

  const other = JSON.parse(json);
  other.document.name = 'Another RFID';
  await importFile(page, 'other.json', JSON.stringify(other));

  await expect(page.getByRole('heading', { name: 'Replace the open document?' })).toBeVisible();

  await page.getByRole('button', { name: 'Cancel', exact: true }).click();

  await expect(page.locator('#lf-id')).toHaveValue('0102030406');

  await page.evaluate(() =>
    Object.defineProperty(window, 'localStorage', {
      get() {
        throw new Error('Storage unavailable');
      }
    })
  );
  await page.locator('#lf-id').fill('0102030407');

  await expect(page.locator('#studio-target')).toContainText('storage unavailable');
  expect(JSON.parse(await exported(page, '#export-json')).document.memory[0]).toEqual([1, 2, 3, 4, 7]);
});

test('Type 2 scanning keeps partial pages on Stop, and completing a read requires explicit defaults', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'type2' });

  let release;
  const waiting = new Promise((resolve) => {
    page.route('**/api/command/2028?*', async (route) => {
      const body = route.request().postDataBuffer();
      if (body[0] === 5 && body[1] === 8) {
        release = () => route.continue();
        resolve();
      } else {
        await route.continue();
      }
    });
  });
  await page.locator('#scan').click();
  await waiting;
  await page.locator('#stop').click();
  await release();

  await expect(page.locator('#read')).toBeVisible();
  await expect(page.locator('#export')).toBeDisabled();
  await expect(page.locator('#memory-status')).toHaveText('16/45 pages known');

  await page.unroute('**/api/command/2028?*');
  await page.reload();

  await expect(page.locator('#memory-status')).toHaveText('16/45 pages known');

  let versions = 0;
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2028?') && request.postDataBuffer()?.[0] === 0) {
      versions++;
    }
  });
  await expect(page.locator('#type2-profile')).toHaveValue('auto');
  await page.locator('#read').click();
  await page.locator('dialog[open]').getByRole('button', { name: 'Read', exact: true }).click();

  await expect.poll(() => versions).toBe(1);
  await expect(page.getByRole('heading', { name: 'Use the selected profile?' })).toHaveCount(0);

  await expect(page.locator('#stop')).toBeHidden();
  await expect(page.locator('#type2-defaults')).toBeVisible();
  await expect(page.locator('#doc-notes')).toBeVisible();
  await expect(page.locator('#status')).toHaveText('Type 2 read finished');
  await expect(page.locator('#capture-details-dialog')).toBeHidden();
  let details = await captureDetails(page);
  await expect(details).toContainText('original credentials remain unknown');
  await page.keyboard.press('Escape');
  await expect(details).toBeHidden();

  await page.locator('#type2-defaults').click();
  const defaultsDialog = page.getByRole('dialog', { name: 'Fill missing emulator data?' });
  await expect(defaultsDialog).toContainText('Unread user pages will remain unknown.');
  await expect(defaultsDialog).toContainText('Authentication and originality checks may fail.');
  await defaultsDialog.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#type2-defaults')).toBeVisible();
  await expect(page.locator('#export')).toBeDisabled();
  await expect(page.locator('#capture-details-dialog')).toBeHidden();

  await page.locator('#type2-defaults').click();
  await page.getByRole('button', { name: 'Apply defaults', exact: true }).click();
  await expect(page.locator('#toast')).toHaveText('Emulator defaults applied');

  await expect(page.locator('#memory-status')).toHaveText('45/45 pages known');
  await expect(page.locator('#export')).toBeEnabled();
  await expect(page.locator('#doc-notes')).toBeHidden();
  await expect(page.locator('#capture-details-dialog')).toBeHidden();
  details = await captureDetails(page);
  await expect(details).toContainText('PACK becomes 0000');
  await expect(details).toContainText('missing originality data are not reproduced');
  await details.getByRole('button', { name: 'Close', exact: true }).click();
  await expect(details).toBeHidden();
  await expect(page.locator('#status')).toBeEmpty();
  await expect(page.locator('#read-elapsed')).toBeHidden();
  await expect(page.locator('#write-changes')).toHaveText('Write changes (0)');

  await page.reload();
  await expect(page.locator('#doc-notes')).toBeHidden();
  await expect(page.locator('#capture-details-dialog')).toBeHidden();
  details = await captureDetails(page);
  await expect(details).toContainText('PACK becomes 0000');
});

test('slot upload verifies the image, reopening retains edits, and an empty place imports directly', async ({
  page
}) => {
  await scan(page);

  const json = await exported(page, '#export-json');
  await page.locator('#save-menu summary').click();
  await page.locator('#save-slot').click();
  await page.getByRole('button', { name: 'Slot 2 Empty place' }).click();
  await page.getByRole('textbox', { name: 'Name on slot 2' }).fill('Browser test');
  await page.getByRole('button', { name: 'Save', exact: true }).click();

  await expect(page.locator('#status')).toHaveText('Saved and verified on slot 2');
  await expect(page.locator('#read-elapsed')).toBeHidden();
  await expect(page.locator('#read-progress')).toBeHidden();

  const memory = page.locator('#memory textarea');
  const edited = (await memory.inputValue()).replace('00000000', '01020304');
  await memory.fill(edited);
  await page.getByRole('button', { name: 'Slots', exact: true }).click();
  await page.locator('#slot-1 .bay-open').first().click();

  await expect(memory).toHaveValue(edited);

  await page.getByRole('button', { name: 'Slots', exact: true }).click();

  const picker = page.waitForEvent('filechooser');
  await page.locator('#slot-2 .bay-open').first().click();
  await (await picker).setFiles({
    name: 'capture.json',
    mimeType: 'application/json',
    buffer: Buffer.from(json)
  });
  await page.getByRole('button', { name: 'Save', exact: true }).click();

  await expect(page.locator('#slot-2 .bay-open').first()).toContainText('Tag-01020304');

  await page.getByRole('button', { name: 'Studio', exact: true }).click();

  await expect(memory).toHaveValue(edited);
});

test('failed refreshes report the error and allow another attempt', async ({ page }) => {
  await page.getByRole('button', { name: 'Device', exact: true }).click();
  await page.route('**/api/device', (route) => route.fulfill({ status: 503 }));
  await page.locator('#refresh-device').click();

  await expect(page.locator('#toast')).toContainText('Refresh failed:');
  await expect(page.locator('#refresh-device')).toBeEnabled();

  await page.unroute('**/api/device');
  await page.locator('#refresh-device').click();

  await expect(page.locator('#toast')).toHaveText('Device refreshed');
});

test('a failed slot commit releases the session and never reports success', async ({ page }) => {
  await scan(page);
  await page.route('**/api/slots/commit?*', (route) => route.fulfill({ body: Buffer.from([0, 0x70]) }));
  await page.locator('#save-menu summary').click();
  await page.locator('#save-slot').click();
  await page.getByRole('button', { name: 'Slot 2 Empty place' }).click();
  await page.getByRole('button', { name: 'Save', exact: true }).click();

  await expect(page.locator('#toast')).toContainText('Slot change unconfirmed');
  await expect(page.locator('#read')).toBeEnabled();

  await page.getByRole('button', { name: 'Scan', exact: true }).click();
  await scan(page);

  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
});

test('MCT identity confirmation preserves the chosen slot and cancellation clears the import', async ({
  page
}) => {
  await scan(page);

  const mct = await exported(page, '#export-mct');
  await page.getByRole('button', { name: 'Slots', exact: true }).click();

  const picker = page.waitForEvent('filechooser');
  await page.locator('#slot-1 .bay-open').first().click();
  await (await picker).setFiles({ name: 'tag.mct', mimeType: 'text/plain', buffer: Buffer.from(mct) });

  await expect(page.getByRole('heading', { name: 'Confirm MCT identity' })).toBeVisible();

  await page.locator('#mct-dialog').getByRole('button', { name: 'Import', exact: true }).click();

  await expect(page.getByRole('heading', { name: 'Name on slot 2' })).toBeVisible();

  await page.getByRole('button', { name: 'Cancel', exact: true }).click();

  await expect(page.locator('#slot-1 .bay-open').first()).toContainText('Import a tag');

  await page.getByRole('button', { name: 'Studio', exact: true }).click();

  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
});

test('MCT Escape after a completed import preserves the open document', async ({ page }) => {
  await scan(page);
  const mct = await exported(page, '#export-mct');
  const dialog = page.locator('#mct-dialog');

  await importFile(page, 'first.mct', mct);
  await expect(dialog).toBeVisible();
  await dialog.getByRole('button', { name: 'Import', exact: true }).click();
  await expect(page.locator('#studio-name')).toHaveText('first');
  await expect
    .poll(() => page.evaluate((key) => JSON.parse(localStorage.getItem(key)).document.name, draftKey))
    .toBe('first');
  const draft = await page.evaluate((key) => localStorage.getItem(key), draftKey);

  await importFile(page, 'cancelled.mct', mct);
  await expect(dialog).toBeVisible();
  await page.keyboard.press('Escape');

  await expect(dialog).toBeHidden();
  await expect(dialog).toHaveJSProperty('returnValue', '');
  await expect(page.locator('#studio-name')).toHaveText('first');
  expect(JSON.parse(await exported(page, '#export-json')).document.name).toBe('first');
  expect(await page.evaluate((key) => localStorage.getItem(key), draftKey)).toBe(draft);
});

for (const width of [1280, 360, 320]) {
  test(`RFID chip summary and verified copy fit the Studio at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 820 });
    await page.route('**/api/device', async (route) => {
      const details = await (await route.fetch()).json();
      details.battery_percent = 100;
      await route.fulfill({ json: details });
    });
    await page.locator('[data-scan-mode=rfid]').click();
    await scan(page);
    await expect(page.locator('#connection')).toHaveText('100%');
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await expect(page.getByRole('button', { name: 'Export JSON', exact: true })).toBeVisible();
    await expect(page.locator('#export-menu')).toBeHidden();
    const chip = page.locator('#lf-chip');
    const details = chip.locator('details');
    const info = chip.getByLabel('RFID chip details', { exact: true });
    const capability = chip.getByText('T55x7 · Writing possible', { exact: true });
    await expect(capability).toBeVisible();
    if (width === 1280) {
      const identityBounds = await page.locator('#card-value').boundingBox();
      const capabilityBounds = await capability.boundingBox();
      expect(Math.abs(identityBounds.y - capabilityBounds.y)).toBeLessThan(1);
    }
    await expect(info).toHaveText('ⓘ');
    await expect(page.locator('#status')).toBeEmpty();
    await capability.click();
    await expect(details).toHaveJSProperty('open', false);
    await expect(chip.getByText('Manchester · RF/64', { exact: true })).toBeHidden();
    await page.screenshot({
      path: test.info().outputPath(`chameleon-lf-summary-${width}-${test.info().project.name}.png`)
    });
    await info.focus();
    await info.press('Enter');
    await expect(chip.getByText('Manchester · RF/64', { exact: true })).toBeVisible();
    await expect(chip.getByText('Disabled', { exact: true })).toBeVisible();
    await expect(
      chip.getByText('The target is checked again before writing.', { exact: true })
    ).toBeVisible();
    const bounds = await chip.locator('.tag-detail-content').boundingBox();
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(width);
    await expect(page.locator('#lf-hint')).not.toContainText('Recorded radio clock');
    await page.screenshot({
      path: test.info().outputPath(`chameleon-lf-details-${width}-${test.info().project.name}.png`)
    });
    await info.press('Escape');
    await expect(details).toHaveJSProperty('open', false);
    await expect(info).toBeFocused();
    await info.click();
    await page.locator('#lf-id').click();
    await expect(details).toHaveJSProperty('open', false);
    await expect(page.locator('#studio-family')).toHaveText('EM410x');

    await expect(page.locator('#write-changes')).toBeHidden();
    await page.locator('#save-menu summary').click();
    await expect(page.locator('#copy-tag')).toHaveText('Program a tag…');
    await expect(page.locator('#copy-tag-divider')).toBeVisible();
    await page.locator('#copy-tag').click();
    await expect(page.getByRole('heading', { name: 'Program a tag', exact: true })).toBeVisible();
    await page.getByRole('button', { name: 'Check target', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Program this target?' })).toBeVisible();
    const target = page.locator('dialog[open] .lf-chip');
    await expect(target.getByText('T55x7 · Writing possible', { exact: true })).toBeVisible();
    await expect(target.locator('details')).toHaveJSProperty('open', false);
    await target.getByLabel('RFID chip details', { exact: true }).click();
    await expect(target.getByText('Manchester · RF/64', { exact: true })).toBeVisible();
    const targetBounds = await target.locator('.tag-detail-content').boundingBox();
    expect(targetBounds.x).toBeGreaterThanOrEqual(0);
    expect(targetBounds.x + targetBounds.width).toBeLessThanOrEqual(width);
    await page.screenshot({
      path: test.info().outputPath(`chameleon-lf-copy-details-${width}-${test.info().project.name}.png`)
    });
    await target.getByLabel('RFID chip details', { exact: true }).press('Escape');
    await expect(target.locator('details')).toHaveJSProperty('open', false);
    await expect(page.getByRole('heading', { name: 'Program this target?' })).toBeVisible();
    const layout = await page.locator('dialog[open]').evaluate((dialog) => {
      const box = dialog.getBoundingClientRect();
      const buttons = [...dialog.querySelectorAll('button')].map((button) => {
        const rect = button.getBoundingClientRect();
        return {
          x: rect.x,
          right: rect.right,
          height: rect.height,
          scroll: button.scrollWidth,
          client: button.clientWidth
        };
      });
      return {
        x: box.x,
        right: box.right,
        width: innerWidth,
        scroll: dialog.scrollWidth,
        client: dialog.clientWidth,
        buttons
      };
    });
    expect(layout.x).toBeGreaterThanOrEqual(0);
    expect(layout.right).toBeLessThanOrEqual(layout.width);
    expect(layout.scroll).toBeLessThanOrEqual(layout.client);
    expect(layout.buttons[0].right).toBeLessThan(layout.buttons[1].x);
    for (const button of layout.buttons) {
      expect(button.height).toBeGreaterThanOrEqual(40);
      expect(button.scroll).toBeLessThanOrEqual(button.client);
    }
    await page.screenshot({ path: test.info().outputPath(`chameleon-lf-copy-${width}.png`) });
    await page.getByRole('button', { name: 'Program & verify', exact: true }).click();
    await expect(page.locator('#status')).toContainText('written and verified');
    await expect(page.locator('#stop')).toBeHidden();
    await expect(page.locator('#lf-id')).toHaveValue('0102030405');

    await page.locator('#save-menu summary').click();
    await page.locator('#copy-tag').click();
    await page.getByRole('button', { name: 'Check target', exact: true }).click();
    await expect(page.locator('#status')).toContainText('already matches');
    await expect(page.locator('#toast')).toHaveText('Tag already matches');
    await expect(page.locator('dialog[open]')).toHaveCount(0);
    await page.reload();
    await expect(capability).toBeVisible();
    await expect(details).toHaveJSProperty('open', false);
  });
}

test('an uncertain RFID programming reply is retained as an error and never retried', async ({ page }) => {
  await page.locator('[data-scan-mode=rfid]').click();
  await scan(page);
  let programs = 0;
  await page.route('**/api/command/3033?*', async (route) => {
    if (route.request().postDataBuffer()[1] === 2) {
      programs++;
      await route.abort('connectionreset');
    } else {
      await route.continue();
    }
  });
  await page.locator('#save-menu summary').click();
  await page.locator('#copy-tag').click();
  await page.getByRole('button', { name: 'Check target', exact: true }).click();
  await page.getByRole('button', { name: 'Program & verify', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Programming may be incomplete');
  await expect(page.locator('#toast')).toHaveClass('toast visible error');
  await expect(page.locator('#stop')).toBeHidden();
  expect(programs).toBe(1);
});

test('a Viking scan retains its ID, format and RF/32 chip observation after reload', async ({ page }) => {
  await page.route('**/api/command/3000?*', (route) => route.fulfill({ body: Buffer.from([0, 0x41]) }));
  await page.route('**/api/command/3004?*', (route) =>
    route.fulfill({ body: Buffer.from([0, 0x40, 0x09, 0x87, 0x65, 0x43]) })
  );
  await page.route('**/api/command/3033?*', (route) => {
    const reply = Buffer.alloc(38);
    reply.set([0, 0x40, 1, 1, 127, 0]);
    [0x00088041, 0xf2000009, 0x876543f2, 0, 0, 0, 0].forEach((word, index) =>
      reply.writeUInt32BE(word, 6 + index * 4)
    );
    return route.fulfill({ body: reply });
  });
  await page.locator('[data-scan-mode=rfid]').click();
  await scan(page);
  await expect(page.locator('#studio-family')).toHaveText('Viking');
  await expect(page.locator('#card-label')).toHaveText('ID');
  await expect(page.locator('#card-value')).toHaveText('09876543');
  await expect(page.locator('#lf-id')).toHaveValue('09876543');
  await expect(page.locator('#doc-notes')).toBeHidden();
  await expect(page.locator('#save-menu')).toBeVisible();
  await expect(page.locator('#more-menu')).toBeHidden();
  await page.reload();
  await expect(page.locator('#studio-family')).toHaveText('Viking');
  await expect(page.locator('#lf-id')).toHaveValue('09876543');
  await page.locator('#lf-chip summary').click();
  await expect(page.locator('#lf-chip')).toContainText('Manchester · RF/32');
});

test('an undecoded T55xx opens as a chip-only document and survives reload', async ({ page }) => {
  for (const command of [3000, 3002, 3004, 3010, 3014, 3019]) {
    await page.route(`**/api/command/${command}?*`, (route) =>
      route.fulfill({ body: Buffer.from([0, 0x41]) })
    );
  }
  await page.locator('[data-scan-mode=rfid]').click();
  await scan(page);
  await expect(page.locator('#studio-family')).toHaveText('RFID · chip only');
  await expect(page.locator('#card-value')).toHaveText('No decoded ID');
  await expect(page.locator('#lf-chip')).toContainText('T55x7 · Writing possible');
  await expect(page.locator('#save-menu')).toBeHidden();
  await expect(page.locator('#status')).toBeEmpty();
  await expect(page.locator('#read-progress')).toBeHidden();
  const details = await captureDetails(page);
  await expect(details).toContainText('Chip detected; no supported broadcast ID decoded');
  await details.getByRole('button', { name: 'Close', exact: true }).click();
  await page.reload();
  await expect(page.locator('#card-value')).toHaveText('No decoded ID');
  await expect(page.locator('#lf-chip')).toContainText('T55x7 · Writing possible');
  await expect(page.locator('#read-progress')).toBeHidden();
});

test('older firmware keeps the captured RFID ID and explains the unavailable chip check', async ({
  page
}) => {
  await page.route('**/api/command/3033?*', (route) => route.fulfill({ status: 404, body: '' }));
  await page.locator('[data-scan-mode=rfid]').click();
  await scan(page);
  await expect(page.locator('#lf-id')).toHaveValue('0102030405');
  await expect(page.locator('#lf-chip')).toContainText('Chip not identified · Writing unknown');
  await expect(page.locator('#status')).toContainText('Update Chameleon firmware');
  await expect(page.locator('#export-json')).toBeEnabled();
});

for (const [configuration, state, password] of [
  [null, 'Writing unknown', null],
  [0x00148051, 'Writing unsupported', 'Enabled'],
  [0x00148241, 'Writing unsupported', 'Disabled']
]) {
  test(`RFID chip summary retains uncertainty and protection for configuration ${configuration}`, async ({
    page
  }) => {
    await page.route('**/api/command/3033?*', (route) => {
      const reply = Buffer.alloc(38);
      reply.set([0, 0x40, 1]);
      if (configuration !== null) {
        reply.set([1, 127, 0], 3);
        [configuration, 0xff800000, 0x0034a800, 0, 0, 0, 0].forEach((word, index) =>
          reply.writeUInt32BE(word, 6 + index * 4)
        );
      }
      return route.fulfill({ body: reply });
    });
    await page.locator('[data-scan-mode=rfid]').click();
    await scan(page);
    await expect(page.locator('#lf-chip')).toContainText(state);
    await expect(page.locator('#status')).toBeEmpty();
    await page.locator('#lf-chip summary').click();
    if (password) {
      await expect(page.locator('#lf-chip').getByText(password, { exact: true })).toBeVisible();
    } else {
      await expect(page.locator('#lf-chip')).toContainText('Detection was inconclusive');
    }
    await expect(page.locator('#lf-id')).toHaveValue('0102030405');
  });
}

test('Ultralight C copy requests a masked target key and writes only user data', async ({ page }) => {
  const call = await createEngine(
    await readFile(
      new URL('../../../../.cache/web/wasm/wasm32-unknown-unknown/release/nfc_wasm.wasm', import.meta.url)
    )
  );
  const image = {
    model: 19,
    uid: [4, 1, 2, 3, 4, 5, 6],
    memory: Array(192).fill(0),
    metadata: [],
    secrets: []
  };
  image.memory[20 * 4] = 42;
  const { text } = call({ op: 'export_flipper', image });
  const memory = Array.from({ length: 44 }, () => [0, 0, 0, 0]);
  memory[40] = [2, 0, 0, 0];
  memory[42] = [8, 0, 0, 0];
  const key = Array.from({ length: 16 }, (_, i) => i);
  let writes = 0;
  await page.route('**/api/command/**', async (route) => {
    const command = Number(new URL(route.request().url()).pathname.split('/').at(-1));
    if (![2000, 2030, 2031].includes(command)) {
      return route.continue();
    }
    let status = 0;
    let response;
    if (command === 2000) {
      response = [7, ...image.uid, 0, 68, 0, 0];
    } else {
      const payload = Array.from(route.request().postDataBuffer());
      expect(payload.slice(0, 9)).toEqual([19, 7, ...image.uid]);
      if (command === 2031) {
        expect(payload.length).toBe(27);
        const index = payload[9];
        expect(index).toBeLessThan(44);
        if (payload[10] === 0 && index >= 8) {
          status = 2;
          response = [];
        } else {
          if (payload[10] === 4) {
            expect(payload.slice(11)).toEqual(key);
          }
          response = [...memory[index], ...Array(12).fill(255)];
        }
      } else {
        expect(payload.length).toBe(59);
        expect(payload[9]).toBe(20);
        expect(payload[10]).toBe(4);
        expect(payload.slice(11, 27)).toEqual(key);
        response = payload.slice(43, 47);
        expect(response).toEqual([42, 0, 0, 0]);
        writes++;
      }
    }
    await route.fulfill({
      body: Buffer.from([0, status, ...response]),
      contentType: 'application/octet-stream'
    });
  });
  await importFile(page, 'ultralight-c.nfc', text);
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  await expect(page.locator('#studio-document')).toBeVisible();
  await expect(page.locator('#write-changes')).toBeDisabled();
  await page.locator('#save-menu summary').click();
  await expect(page.locator('#copy-tag')).toHaveText('Copy to a tag…');
  await expect(page.locator('#copy-tag-divider')).toBeVisible();
  await expect(page.locator('#copy-tag')).toBeEnabled();
  await page.locator('#copy-tag').click();
  const keyInput = page.getByLabel('Target 3DES key');
  await expect(keyInput).toHaveAttribute('type', 'password');
  await keyInput.fill('000102030405060708090A0B0C0D0E0');
  await expect(page.getByRole('button', { name: 'Use 3DES key', exact: true })).toBeDisabled();
  await keyInput.fill('000102030405060708090A0B0C0D0E0F');
  await page.getByRole('button', { name: 'Use 3DES key', exact: true }).click();
  await page.getByRole('button', { name: 'Copy data', exact: true }).click();
  await expect(page.locator('#status')).toContainText('1 page written and verified');
  expect(writes).toBe(1);
});

for (const [type, code] of [
  ['Static', 0],
  ['Weak', 1],
  ['Hard', 2]
]) {
  test(`Classic PRNG ${type} is visible on the card without opening details`, async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 800 });
    await page.route('**/api/command/2040?*', (route) =>
      route.fulfill({ body: Buffer.from([0, 0, 0, 0, 2, code, 1, ...Array(6).fill(255)]) })
    );
    await scan(page, false);
    const badge = page.locator('#classic-prng');
    await expect(badge).toBeVisible();
    await expect(badge).toHaveText(`PRNG ${type}`);
    await expect(page.locator('#classic-evidence')).toBeHidden();
    const bounds = await badge.boundingBox();
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(360);
    await page.screenshot({
      path: test.info().outputPath(`chameleon-prng-${type}-${test.info().project.name}.png`),
      fullPage: true
    });
  });
}

test('Classic reads preserve scan inspection without repeating magic probes', async ({ page }) => {
  const inspections = [];
  page.on('request', (request) => {
    if (/\/api\/command\/(2032|2040)\?/.test(request.url())) {
      inspections.push(Number(new URL(request.url()).pathname.split('/').at(-1)));
    }
  });
  await page.route('**/api/command/2040?*', (route) =>
    route.fulfill({ body: Buffer.from([0, 0, 0, 0, 2, 2, 1, ...Array(6).fill(255)]) })
  );
  await scan(page, false);
  await expect(page.locator('#classic-physical')).toHaveText('UID write Unknown');
  await expect(page.locator('#classic-prng')).toBeVisible();
  await expect(page.locator('#classic-prng')).toHaveText('PRNG Hard');

  for (const reread of [false, true]) {
    await page.locator('#read').click();
    if (reread) {
      await page.getByRole('button', { name: 'Recover', exact: true }).click();
    }
    await expect(page.locator('#status')).toHaveText('Read complete');
    await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
    await expect(page.locator('#classic-physical')).toHaveText('UID write Unknown');
    await expect(page.locator('#classic-prng')).toBeVisible();
    await expect(page.locator('#classic-prng')).toHaveText('PRNG Hard');
    expect(inspections).toEqual([2040]);
  }
});

test('physical magic observations stay separate from slot emulator settings and imported dumps', async ({
  page
}) => {
  await scan(page);
  await expect(page.locator('#classic-physical')).toHaveText('UID write Gen1A · Writable');
  await expect(page.locator('#classic-emulation')).toBeHidden();
  const json = await exported(page, '#export-json');
  expect(JSON.parse(json).document.classicObservation).toBeUndefined();

  let probes = 0;
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2032?')) {
      probes++;
    }
  });
  await page.getByRole('button', { name: 'Slots', exact: true }).click();
  await page.locator('#slot-0 .bay-open').first().click();
  await expect(page.locator('#classic-physical')).toHaveText('UID write Not checked');
  await expect(page.locator('#classic-prng')).toBeHidden();
  await expect(page.locator('#classic-emulation')).toBeHidden();
  await page.getByLabel('UID detection details', { exact: true }).click();
  await expect(page.locator('#classic-emulation')).toHaveText('Emulation · Gen1A disabled · Gen2 disabled');
  await page.getByLabel('UID detection details', { exact: true }).click();
  expect(probes).toBe(0);
  await expect(page.locator('#studio-target')).toContainText('Slot 1');
  await page.screenshot({
    path: test.info().outputPath(`chameleon-classic-slot-${test.info().project.name}.png`),
    fullPage: true
  });
  await page.locator('#more-menu summary').click();
  await expect(page.getByRole('button', { name: 'Change tag UID…', exact: true })).toBeVisible();
  await page.locator('#more-menu summary').click();
  await expect(page.locator('#save-menu')).not.toContainText('Change');

  await importFile(page, 'capture.json', json);
  await expect(page.locator('#classic-physical')).toHaveText('UID write Not checked');
  await expect(page.locator('#classic-prng')).toBeHidden();
  await expect(page.locator('#classic-emulation')).toBeHidden();
  expect(probes).toBe(0);
});

test('editing a physical UID invalidates its magic observation and reload does not restore it', async ({
  page
}) => {
  await scan(page);
  await expect(page.locator('#classic-physical')).toContainText('Gen1A');
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  await memory.fill((await memory.inputValue()).replace('01020304', 'AABBCCDD'));
  await expect(page.locator('#classic-physical')).toHaveText('UID write Not checked');
  await expect(page.locator('#classic-prng')).toBeHidden();
  await page.reload();
  await expect(page.locator('#classic-physical')).toHaveText('UID write Not checked');
  await expect(page.locator('#classic-prng')).toBeHidden();
});

test('Gen2 confirmation is available after scan and retained by explicit Read', async ({ page }) => {
  const credentials = [];
  await page.route('**/api/command/2032?*', (route) => {
    const credential = route.request().postDataBuffer()[14];
    credentials.push(credential);
    return route.fulfill({ body: Buffer.from([0, 0x69]) });
  });
  await page.route('**/api/command/2040?*', (route) => {
    credentials.push('inspection');
    return route.fulfill({ body: Buffer.from([0, 0, 0, 4, 3, 1, 1, ...Array(6).fill(255)]) });
  });
  await scan(page, false);
  await expect(page.locator('#classic-physical')).toHaveText('UID write CUID · Writable');
  expect(credentials).toEqual(['inspection']);
  await expect(page.locator('#classic-evidence')).toBeHidden();
  await page.getByLabel('UID detection details', { exact: true }).click();
  await expect(page.locator('#classic-evidence')).toHaveText('Gen2 / CUID confirmed · no data written.');
  await page.keyboard.press('Tab');
  await page.getByLabel('UID detection details', { exact: true }).click();
  await expect(page.locator('#classic-activation')).toBeHidden();
  await page.screenshot({
    path: test.info().outputPath(`chameleon-classic-magic-${test.info().project.name}.png`),
    fullPage: true
  });
  await expect(page.locator('#identity-title')).toHaveText('Memory not read');
  await page.setViewportSize({ width: 360, height: 800 });
  await page.getByLabel('UID detection details', { exact: true }).click();
  await expect(page.locator('#classic-evidence')).toBeVisible();
  const bounds = await page.locator('#classic-details .tag-detail-content').boundingBox();
  expect(bounds.x).toBeGreaterThanOrEqual(0);
  expect(bounds.x + bounds.width).toBeLessThanOrEqual(360);
  await page.getByLabel('UID detection details', { exact: true }).click();
  await page.locator('#read').click();
  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
  await expect(page.locator('#classic-physical')).toHaveText('UID write CUID · Writable');
  expect(credentials).toEqual(['inspection']);
});

test('scan duration includes identification and cleanup, then freezes', async ({ page }) => {
  let scanRequestedAt;
  let cleanupRespondedAt;
  await page.route('**/api/command/2000?*', async (route) => {
    if (scanRequestedAt === undefined) {
      scanRequestedAt = await page.evaluate(() => performance.now());
      await new Promise((resolve) => setTimeout(resolve, 650));
    }
    await route.continue();
  });
  await page.route('**/api/end?*', async (route) => {
    await new Promise((resolve) => setTimeout(resolve, 650));
    const response = await route.fetch();
    cleanupRespondedAt = await page.evaluate(() => performance.now());
    await route.fulfill({ response });
  });
  await scan(page, false);
  const elapsed = page.getByLabel('Total read time', { exact: true });
  const frozen = await elapsed.textContent();
  const seconds = Number.parseFloat(frozen);
  expect(seconds).toBeGreaterThanOrEqual(1.3);
  const observedSeconds = (cleanupRespondedAt - scanRequestedAt) / 1000;
  expect(seconds).toBeGreaterThan(observedSeconds - 0.1);
  expect(seconds).toBeLessThan(observedSeconds + 0.5);
  await expect(page.locator('#status')).toHaveText('Tag identified · Memory not read');
  await expect(page.locator('#read-progress')).toBeHidden();
  await page.waitForTimeout(250);
  await expect(elapsed).toHaveText(frozen);
});

test('Classic Write changes tracks undo, confirms blocks and writes without a full reread', async ({
  page
}) => {
  await scan(page);
  const write = page.locator('#write-changes');
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const original = await memory.inputValue();
  const lines = original.split('\n');
  await expect(write).toHaveText('Write changes (0)');
  await expect(write).toBeDisabled();
  lines[1] = '11'.repeat(16);
  lines[4] = '22'.repeat(16);
  await memory.fill(lines.join('\n'));
  await expect(write).toHaveText('Write changes (2)');
  await expect(write).toBeEnabled();
  await memory.fill(original);
  await expect(write).toHaveText('Write changes (0)');
  await memory.fill(lines.join('\n'));

  const commands = [];
  page.on('request', (request) => {
    const command = request.url().match(/\/api\/command\/(\d+)/)?.[1];
    if (command) {
      commands.push(Number(command));
    }
  });
  await write.click();
  let confirmation = page.getByRole('dialog', { name: 'Write changes?', exact: true });
  await expect(confirmation).toContainText('Tag 01020304 · 2 data blocks: 1, 4.');
  await confirmation.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#stop')).toBeHidden();
  expect(commands).toEqual([]);
  await expect(page.locator('#toast')).not.toHaveClass(/visible/);
  await expect(write).toBeEnabled();

  await page.setViewportSize({ width: 360, height: 800 });
  expect(await page.locator('body').evaluate((body) => body.scrollWidth)).toBeLessThanOrEqual(360);
  await page.screenshot({
    path: test.info().outputPath(`chameleon-classic-write-${test.info().project.name}.png`),
    fullPage: true
  });
  await write.click();
  confirmation = page.getByRole('dialog', { name: 'Write changes?', exact: true });
  await confirmation.getByRole('button', { name: 'Write changes', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('2 blocks written and verified');
  await expect(page.locator('#toast')).toHaveText('Changes written and verified');
  await expect(page.locator('#toast')).toHaveClass('toast visible');
  await expect(page.locator('#stop')).toBeHidden();
  await expect(write).toHaveText('Write changes (0)');
  expect(commands).toEqual([2000, 2030, 2030]);

  await page.locator('#read').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Recover', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('Read complete');
  await expect(page.locator('#stop')).toBeHidden();
  const reread = (await memory.inputValue()).split('\n');
  expect(reread[1].replace(/\s/g, '')).toBe('11'.repeat(16));
  expect(reread[4].replace(/\s/g, '')).toBe('22'.repeat(16));
  const saved = JSON.parse(await exported(page, '#export-json'));
  expect(saved.document.classicRead).toBeUndefined();
  await page.reload();
  await expect(write).toBeVisible();
  await expect(write).toHaveText('Write changes');
  await expect(write).toBeDisabled();
  await expect(write).toHaveAttribute('title', /Read this tag in the current session/);
});

test('Classic Write changes accepts a partial read, skips block 0 and reports unknown edits', async ({
  page
}) => {
  let release;
  const waiting = new Promise((resolve) => {
    page.route('**/api/command/2027?*', async (route) => {
      release = () => route.continue();
      resolve();
    });
  });
  await scan(page, false);
  await page.locator('#read').click();
  await waiting;
  await page.locator('#stop').click();
  await release();
  await expect(page.locator('#stop')).toBeHidden();
  await expect(page.locator('#memory-status')).toHaveText('4/64 blocks known');
  await page.unroute('**/api/command/2027?*');

  const write = page.locator('#write-changes');
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const lines = (await memory.inputValue()).split('\n');
  const unknown = lines[4];
  lines[0] = '00'.repeat(16);
  lines[1] = '11'.repeat(16);
  await memory.fill(lines.join('\n'));
  await expect(write).toHaveText('Write changes (1)');
  await expect(write).toBeEnabled();
  lines[4] = '22'.repeat(16);
  await memory.fill(lines.join('\n'));
  await expect(write).toBeDisabled();
  await expect(page.locator('#doc-notes-text')).toContainText(
    'complete original and edited bytes for blocks 4'
  );
  lines[4] = unknown;
  await memory.fill(lines.join('\n'));
  await write.click();
  const confirmation = page.getByRole('dialog', { name: 'Write changes?', exact: true });
  await expect(confirmation).toContainText('Tag 01020304');
  await expect(confirmation).not.toContainText('excluded');
  await confirmation.getByRole('button', { name: 'Write changes', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('1 block written and verified');
  await expect(write).toHaveText('Write changes (0)');
});

test('Classic Stop retains a confirmed block and requires a fresh read before retrying', async ({ page }) => {
  await scan(page);
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const lines = (await memory.inputValue()).split('\n');
  lines[1] = '11'.repeat(16);
  lines[2] = '22'.repeat(16);
  await memory.fill(lines.join('\n'));
  let release;
  const writes = [];
  const waiting = new Promise((resolve) => {
    page.route('**/api/command/2030?*', async (route) => {
      writes.push(route.request().postDataBuffer()[9]);
      release = () => route.continue();
      resolve();
    });
  });
  await page.locator('#write-changes').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Write changes', exact: true }).click();
  await waiting;
  await page.locator('#stop').click();
  await release();
  await expect(page.locator('#stop')).toBeHidden();
  await expect(page.locator('#status')).toContainText('1/2 blocks written and verified');
  await expect(page.locator('#status')).toContainText('Read the tag again before writing');
  await expect(page.locator('#write-changes')).toBeDisabled();
  expect(writes).toEqual([1]);
  expect((await memory.inputValue()).split('\n')[2]).toBe(lines[2]);
});

for (const mode of ['Gen1A', 'CUID', 'unknown']) {
  test(`UID action is in More actions and explains the actual write method (${mode})`, async ({ page }) => {
    await page.route('**/api/command/2032?*', (route) => {
      const kind = route.request().postDataBuffer()[14];
      let reply = [0, 0, 0, 0, 0];
      if (mode === 'Gen1A') {
        reply = [0, 0, 0, 1, 0];
      } else if (mode === 'CUID' && kind) {
        reply = [0, 0, 0, 4, 3];
      }
      return route.fulfill({ body: Buffer.from(reply) });
    });
    await scan(page);
    const writes = [];
    page.on('request', (request) => {
      if (request.url().includes('/api/command/2033?')) {
        writes.push(request.url());
      }
    });
    await page.setViewportSize({ width: 360, height: 800 });
    await page.getByLabel('More actions', { exact: true }).click();
    await page.getByRole('button', { name: 'Change tag UID…', exact: true }).click();
    const dialog = page.locator('dialog[open]');
    await expect(dialog).toContainText('Current UID: 01020304');
    await dialog.getByRole('textbox', { name: 'New UID', exact: true }).fill('AABBCCDD');
    if (mode === 'CUID') {
      await expect(dialog).toContainText('CUID detected');
      await expect(dialog).not.toContainText('one-time');
      await expect(dialog.getByRole('textbox', { name: 'Sector-0 key', exact: true })).toHaveValue(
        'FFFFFFFFFFFF'
      );
      await expect(dialog.getByRole('button', { name: 'Change UID', exact: true })).toBeEnabled();
    } else if (mode === 'Gen1A') {
      await expect(dialog).toContainText('No key is required');
      await expect(dialog.getByRole('textbox', { name: 'Sector-0 key', exact: true })).toHaveCount(0);
      await expect(dialog.getByRole('button', { name: 'Change UID', exact: true })).toBeEnabled();
    } else {
      await expect(dialog).toContainText('UID writability is unconfirmed');
      await expect(dialog).toContainText('one-time');
      await expect(dialog.getByRole('button', { name: 'Write UID with key', exact: true })).toBeDisabled();
    }
    expect(await page.locator('body').evaluate((body) => body.scrollWidth)).toBeLessThanOrEqual(360);
    await page.screenshot({
      path: test.info().outputPath(`chameleon-uid-dialog-${mode}-${test.info().project.name}.png`)
    });
    await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(page.locator('#stop')).toBeHidden();
    expect(writes).toEqual([]);
  });
}

test('Classic Write changes describes key edits, writes data before trailers and requires a fresh read', async ({
  page
}) => {
  await scan(page);
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const lines = (await memory.inputValue()).split('\n');
  lines[1] = '11'.repeat(16);
  // Uniform custom keys exercise rereading without nonce captures in this simulator.
  for (let trailer = 3; trailer < lines.length; trailer += 4) {
    lines[trailer] = 'D7C2A9E46135 FF078069 B6E408C3F27A';
  }
  await memory.fill(lines.join('\n'));
  await expect(page.locator('#write-changes')).toHaveText('Write changes (17)');
  const writes = [];
  page.on('request', (request) => {
    const command = Number(request.url().match(/\/api\/command\/(\d+)/)?.[1]);
    if (command === 2030 || (command === 2034 && request.postDataBuffer()?.length === 37)) {
      writes.push(command);
    }
  });
  await page.locator('#write-changes').click();
  let dialog = page.getByRole('dialog', { name: 'Write changes?', exact: true });
  await expect(dialog).toContainText('1 data block: 1');
  await expect(dialog).toContainText('Keys and access rights: sectors 0, 1');
  await expect(dialog).toContainText('Read the tag again');
  await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
  expect(writes).toEqual([]);
  await page.locator('#write-changes').click();
  dialog = page.getByRole('dialog', { name: 'Write changes?', exact: true });
  await dialog.getByRole('button', { name: 'Write changes', exact: true }).click();
  await expect(page.locator('#status')).toContainText('17 blocks written and verified');
  await expect(page.locator('#status')).toContainText('Read the tag again');
  await expect(page.locator('#write-changes')).toBeDisabled();
  expect(writes).toEqual([2030, ...Array(16).fill(2034)]);
  const submittedKeys = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2015?')) {
      submittedKeys.push(request.postDataBuffer().subarray(3).toString('hex').toUpperCase());
    }
  });
  await page.locator('#read').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Recover', exact: true }).click();
  await expect(page.locator('#status')).toHaveText('Read complete');
  await expect(page.locator('#write-changes')).toHaveText('Write changes (0)');
  expect(submittedKeys.some((keys) => keys.includes('D7C2A9E46135'))).toBe(true);
  const reread = (await memory.inputValue()).split('\n');
  expect(reread[3].replace(/\s/g, '')).toBe('D7C2A9E46135FF078069B6E408C3F27A');
});

test('Classic rejects a trailer permission failure before writing any data', async ({ page }) => {
  await scan(page);
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const lines = (await memory.inputValue()).split('\n');
  lines[1] = '11'.repeat(16);
  lines[3] = '010203040506 FF078069 060504030201';
  await memory.fill(lines.join('\n'));
  let writes = 0;
  await page.route('**/api/command/2030?*', (route) => {
    writes++;
    return route.continue();
  });
  await page.route('**/api/command/2034?*', (route) => route.fulfill({ body: Buffer.from([0, 0x70]) }));
  await page.locator('#write-changes').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Write changes', exact: true }).click();
  await expect(page.locator('#status')).toContainText('0/2 blocks written and verified');
  await expect(page.locator('#status')).toContainText('Sector 0');
  expect(writes).toBe(0);
});

test('Classic copy animates inspection, counts verified blocks and waits for cleanup before success', async ({
  page
}) => {
  await scan(page);
  await page.setViewportSize({ width: 360, height: 800 });
  const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
  const lines = (await memory.inputValue()).split('\n');
  lines[1] = '11'.repeat(16);
  lines[2] = '22'.repeat(16);
  await memory.fill(lines.join('\n'));

  let releaseInspection;
  const inspection = new Promise((resolve) => {
    releaseInspection = resolve;
  });
  await page.route('**/api/command/2008?*', async (route) => {
    await inspection;
    await route.continue();
  });
  let releaseWrite;
  const writing = new Promise((resolve) => {
    releaseWrite = resolve;
  });
  let writes = 0;
  await page.route('**/api/command/2030?*', async (route) => {
    writes++;
    if (writes === 2) {
      await writing;
    }
    await route.continue();
  });
  let releaseCleanup;
  const cleanup = new Promise((resolve) => {
    releaseCleanup = resolve;
  });
  await page.route('**/api/end?*', async (route) => {
    const response = await route.fetch();
    await cleanup;
    await route.fulfill({ response });
  });

  await page.locator('#save-menu summary').click();
  await page.locator('#copy-tag').click();
  const feedback = page.locator('#copy-progress');
  const bar = page.getByRole('progressbar', { name: 'Verified copy progress' });
  await expect(feedback).toBeVisible();
  await expect(feedback).toHaveAttribute('data-state', 'busy');
  await expect(bar).not.toHaveAttribute('value');
  await expect(feedback).toContainText('Keep the target on the antenna.');
  await expect(page.locator('#copy-elapsed')).toHaveText(/^\d+\.\d s$/);
  await expect(page.locator('#stop')).toBeEnabled();
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.screenshot({
    path: test.info().outputPath(`chameleon-copy-progress-mobile-${test.info().project.name}.png`)
  });
  await page.setViewportSize({ width: 1200, height: 900 });
  await page.screenshot({
    path: test.info().outputPath(`chameleon-copy-progress-desktop-${test.info().project.name}.png`)
  });
  await page.emulateMedia({ reducedMotion: 'reduce' });
  expect(
    await page.locator('.copy-track').evaluate((track) => getComputedStyle(track, '::after').animationName)
  ).toBe('none');
  await page.emulateMedia({ reducedMotion: 'no-preference' });

  releaseInspection();
  const dialog = page.getByRole('dialog', { name: 'Copy to a tag', exact: true });
  await expect(dialog).toBeVisible();
  await expect(feedback).toHaveAttribute('data-state', 'paused');
  expect(
    await page
      .locator('.copy-track')
      .evaluate((track) => getComputedStyle(track, '::after').animationPlayState)
  ).toBe('paused');
  await dialog.getByRole('button', { name: 'Copy data', exact: true }).click();
  await expect(bar).toHaveAttribute('max', '2');
  await expect(bar).toHaveAttribute('value', '1');
  await expect(page.locator('#copy-progress-label')).toHaveText('1 / 2 blocks verified');
  releaseWrite();
  await expect(bar).toHaveAttribute('value', '2');
  await expect(feedback).toHaveAttribute('data-state', 'busy');
  releaseCleanup();
  await expect(feedback).toHaveAttribute('data-state', 'success');
  await expect(page.locator('#copy-progress-label')).toHaveText('Copy verified · 2 / 2 blocks verified');
  await expect(page.locator('#stop')).toBeHidden();
  const frozen = await page.locator('#copy-elapsed').textContent();
  await page.waitForTimeout(250);
  await expect(page.locator('#copy-elapsed')).toHaveText(frozen);
  await page.locator('#read').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Recover', exact: true }).click();
  await expect(feedback).toBeHidden();
  await expect(page.locator('#stop')).toBeHidden();
});

for (const scenario of ['write failure', 'cleanup failure', 'cancel', 'stop', 'stop during cleanup']) {
  test(`Classic copy feedback handles ${scenario}`, async ({ page }) => {
    await scan(page);
    const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
    const lines = (await memory.inputValue()).split('\n');
    lines[1] = '11'.repeat(16);
    lines[2] = '22'.repeat(16);
    await memory.fill(lines.join('\n'));
    let releaseInspection;
    const inspection = new Promise((resolve) => {
      releaseInspection = resolve;
    });
    if (scenario === 'stop') {
      await page.route('**/api/command/2008?*', async (route) => {
        await inspection;
        await route.continue();
      });
    }
    let writes = 0;
    await page.route('**/api/command/2030?*', (route) => {
      writes++;
      if (scenario === 'write failure' && writes === 2) {
        return route.fulfill({ body: Buffer.from([0, 0x70]) });
      }
      return route.continue();
    });
    let releaseCleanup;
    const cleanup = new Promise((resolve) => {
      releaseCleanup = resolve;
    });
    if (scenario === 'cleanup failure' || scenario === 'stop during cleanup') {
      await page.route('**/api/end?*', async (route) => {
        const response = await route.fetch();
        if (scenario === 'cleanup failure') {
          await route.fulfill({ status: 500, body: '' });
        } else {
          await cleanup;
          await route.fulfill({ response });
        }
      });
    }
    await page.locator('#save-menu summary').click();
    await page.locator('#copy-tag').click();
    const feedback = page.locator('#copy-progress');
    if (scenario === 'stop') {
      await expect(feedback).toHaveAttribute('data-state', 'busy');
      await page.locator('#stop').click();
      releaseInspection();
    } else {
      const dialog = page.getByRole('dialog', { name: 'Copy to a tag', exact: true });
      const action = scenario === 'cancel' ? 'Cancel' : 'Copy data';
      await dialog.getByRole('button', { name: action, exact: true }).click();
    }
    if (scenario === 'stop during cleanup') {
      await expect(page.locator('#copy-progress-bar')).toHaveAttribute('value', '2');
      await page.locator('#stop').click();
      releaseCleanup();
    }
    const stopped = ['stop', 'cancel', 'stop during cleanup'].includes(scenario);
    await expect(feedback).toHaveAttribute('data-state', stopped ? 'stopped' : 'error');
    await expect(page.locator('#stop')).toBeHidden();
    if (scenario === 'stop during cleanup') {
      expect(writes).toBe(2);
      await expect(page.locator('#copy-progress-label')).toHaveText('Copy stopped · 2 / 2 blocks verified');
      await expect(page.locator('#status')).toHaveText('Copy stopped · completed writes were verified');
    } else if (stopped) {
      expect(writes).toBe(0);
      await expect(page.locator('#copy-progress-label')).toHaveText('Copy stopped');
    } else {
      const verified = scenario === 'write failure' ? 1 : 2;
      await expect(page.locator('#copy-progress-label')).toHaveText(
        `Copy failed · ${verified} / 2 blocks verified`
      );
      await expect(page.locator('#status')).toContainText(`${verified}/2 blocks written and verified`);
    }
  });
}

for (const scenario of [
  'clone',
  'data',
  'unsupported',
  'activation bytes',
  'fixed UID',
  'protected target'
]) {
  test(`Classic copy offers explicit modes and explains their effects: ${scenario}`, async ({ page }) => {
    await scan(page);
    const memory = page.getByRole('textbox', { name: 'Memory, one block per line in hexadecimal' });
    const lines = (await memory.inputValue()).split('\n');
    lines[0] = (scenario === 'activation bytes' ? 'AABBCCDD00090400' : 'AABBCCDD00080400') + '99'.repeat(8);
    lines[1] = '11'.repeat(16);
    lines[3] = '010203040506 FF078069 060504030201';
    await memory.fill(lines.join('\n'));
    if (scenario === 'unsupported') {
      await page.route('**/api/command/2038?*', (route) => route.fulfill({ body: Buffer.from([0, 0x67]) }));
    }
    if (scenario === 'fixed UID') {
      await page.route('**/api/command/2032?*', (route) =>
        route.fulfill({ body: Buffer.from([0, 0, 0, 0, 2]) })
      );
    }
    if (scenario === 'protected target') {
      await page.route('**/api/command/2008?*', (route) => {
        if (route.request().postDataBuffer()?.[1] === 2) {
          return route.fulfill({ body: Buffer.from([0, 6]) });
        }
        return route.continue();
      });
    }
    const writes = [];
    page.on('request', (request) => {
      const command = Number(request.url().match(/\/api\/command\/(\d+)/)?.[1]);
      const length = request.postDataBuffer()?.length;
      if (command === 2030 || (command === 2034 && length === 37) || (command === 2038 && length === 59)) {
        writes.push(command);
      }
    });
    await page.setViewportSize({ width: 360, height: 800 });
    await page.locator('#save-menu summary').click();
    await page.locator('#copy-tag').click();
    const dialog = page.getByRole('dialog', { name: 'Copy to a tag', exact: true });
    await expect(dialog).toBeVisible();
    await expect(dialog).toContainText('Source UID');
    await expect(dialog).toContainText('AABBCCDD');
    await expect(dialog).toContainText('Target UID');
    await expect(dialog).toContainText('01020304');
    await expect(dialog.getByRole('radio', { name: /^Copy data/ })).toBeChecked();
    const clone = dialog.getByRole('radio', { name: /^Clone tag/ });
    if (['fixed UID', 'unsupported', 'protected target'].includes(scenario)) {
      await expect(clone).toBeDisabled();
      await expect(dialog.getByText('Unavailable', { exact: true })).toBeVisible();
      await expect(dialog.getByRole('button', { name: 'Copy data', exact: true })).toBeEnabled();
      expect(await page.locator('body').evaluate((body) => body.scrollWidth)).toBeLessThanOrEqual(360);
      await page.screenshot({
        path: test
          .info()
          .outputPath(`chameleon-copy-unavailable-${scenario}-${test.info().project.name}-mobile.png`)
      });
      await page.setViewportSize({ width: 1200, height: 900 });
      await page.screenshot({
        path: test
          .info()
          .outputPath(`chameleon-copy-unavailable-${scenario}-${test.info().project.name}-desktop.png`)
      });
    }
    if (scenario === 'fixed UID') {
      await expect(clone).toBeDisabled();
      await expect(dialog).toContainText('Block 0 requires a supported Gen1A or CUID target');
      await dialog.getByRole('button', { name: 'Copy data', exact: true }).click();
      await expect(page.locator('#status')).toContainText('data copy');
      await expect(page.locator('#copy-progress')).toHaveAttribute('data-state', 'success');
      expect(writes).toEqual([2030]);
      return;
    }
    if (scenario === 'unsupported' || scenario === 'protected target') {
      const reason =
        scenario === 'unsupported'
          ? 'Firmware does not support'
          : 'Some target blocks are protected or could not be checked.';
      await expect(dialog).toContainText(reason);
      await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
      expect(writes).toEqual([]);
      return;
    }
    await expect(clone).toBeEnabled();
    await clone.check();
    await expect(dialog.getByRole('button', { name: 'Clone tag', exact: true })).toBeEnabled();
    await expect(dialog).toContainText('Replace the complete block 0');
    await expect(dialog).toContainText('Replace keys and access rights in sectors 0');
    expect(await page.locator('body').evaluate((body) => body.scrollWidth)).toBeLessThanOrEqual(360);
    await page.screenshot({
      path: test.info().outputPath(`chameleon-copy-${scenario}-${test.info().project.name}.png`),
      fullPage: true
    });
    if (scenario === 'clone') {
      await page.setViewportSize({ width: 1200, height: 900 });
      await page.screenshot({
        path: test.info().outputPath(`chameleon-copy-desktop-${test.info().project.name}.png`)
      });
    }
    if (scenario === 'data') {
      await dialog.getByRole('radio', { name: /^Copy data/ }).check();
      await dialog.getByRole('button', { name: 'Copy data', exact: true }).click();
      await expect(page.locator('#status')).toContainText('data copy');
      expect(writes).toEqual([2030]);
    } else {
      await dialog.getByRole('button', { name: 'Clone tag', exact: true }).click();
      await expect(page.locator('#status')).toContainText('Clone verified');
      expect(writes).toEqual([2030, 2038, 2034]);
    }
  });
}

test('Classic scan stops at identity inspection and offers Read and maintenance without a dump', async ({
  page
}) => {
  const commands = [];
  page.on('request', (request) => {
    const match = request.url().match(/\/api\/command\/(\d+)/);
    if (match) {
      commands.push(Number(match[1]));
    }
  });
  await scan(page, false);
  await expect(page.locator('#identity-title')).toHaveText('Memory not read');
  await expect(page.locator('#status')).toHaveText('Tag identified · Memory not read');
  await expect(page.locator('#classic-physical')).toContainText('Gen1A');
  await expect(page.locator('#read')).toBeEnabled();
  expect(commands).not.toContain(2008);
  expect(commands).not.toContain(2027);
  expect(commands).not.toContain(2023);
  await page.locator('#more-menu summary').click();
  await expect(page.getByRole('button', { name: 'Repair tag…', exact: true })).toBeEnabled();
  await expect(page.getByRole('button', { name: 'Factory format tag…', exact: true })).toBeEnabled();
  await page.screenshot({ path: test.info().outputPath('chameleon-classic-scan.png') });
});

test('Factory format previews the physical tag, cancels without writing and verifies after confirmation', async ({
  page
}) => {
  await page.setViewportSize({ width: 360, height: 820 });
  await scan(page, false);
  const operations = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?')) {
      operations.push(request.postDataBuffer()[0]);
    }
  });
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Factory format tag…', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
  await expect(page.locator('[data-effect=identity]')).toHaveText('Preserved');
  await expect(page.locator('[data-changes]')).toHaveText('Data already zero · reset 1 sector');
  expect(operations).not.toContain(2);
  await page.screenshot({ path: test.info().outputPath('chameleon-classic-format-360.png') });
  expect(
    await page.locator('dialog[open]').evaluate((dialog) => dialog.scrollWidth <= dialog.clientWidth)
  ).toBe(true);
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#stop')).toBeHidden();
  expect(operations).not.toContain(2);
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Factory format tag…', exact: true }).click();
  await page.getByRole('button', { name: 'Format card', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Factory format verified');
  await expect(page.locator('#toast')).toHaveText('Factory format verified');
  await expect(page.locator('#toast')).toHaveClass('toast visible');
  expect(operations.filter((operation) => operation === 2)).toHaveLength(1);
  await expect(page.locator('#identity-title')).toHaveText('Memory not read');
});

test('Factory format refuses a denied final trailer before any writes', async ({ page }) => {
  await scan(page, false);
  let writes = 0;
  await page.route('**/api/command/2039?*', async (route) => {
    const data = route.request().postDataBuffer();
    if (data[0] === 2) {
      writes++;
    }
    if (data[0] === 1 && data[11] === 63) {
      await route.fulfill({ body: Buffer.from([0, 0x75]) });
    } else {
      await route.continue();
    }
  });
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Factory format tag…', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Check again', exact: true })).toBeEnabled();
  await expect(page.locator('dialog[open]')).toContainText('Block 63: no usable key or write permission');
  expect(writes).toBe(0);
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#read')).toBeEnabled();
});

test('Factory format retains an uncertain write and never retries it automatically', async ({ page }) => {
  await scan(page, false);
  let writes = 0;
  await page.route('**/api/command/2039?*', async (route) => {
    if (route.request().postDataBuffer()[0] === 2) {
      writes++;
      await route.fetch();
      await route.abort('connectionreset');
    } else {
      await route.continue();
    }
  });
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Factory format tag…', exact: true }).click();
  await page.getByRole('button', { name: 'Format card', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Check again', exact: true })).toBeEnabled();
  await expect(page.locator('dialog[open]')).toContainText('Write outcome unknown');
  expect(writes).toBe(1);
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#read')).toBeEnabled();
  await expect(page.locator('#classic-physical')).toHaveText('UID write Not checked');
});

test('cancelling maintenance during inspection releases the reader and reports the stop', async ({
  page
}) => {
  await scan(page, false);
  let release;
  let started;
  const waiting = new Promise((resolve) => {
    started = resolve;
  });
  const pending = new Promise((resolve) => {
    release = resolve;
  });
  await page.route('**/api/command/2039?*', async (route) => {
    started();
    await pending;
    await route.continue();
  });
  await page.locator('#more-menu summary').click();
  await page.getByRole('button', { name: 'Repair tag…', exact: true }).click();
  await waiting;
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  release();
  await expect(page.locator('#read')).toBeEnabled();
  await expect(page.locator('dialog[open]')).toHaveCount(0);
  await expect(page.locator('#status')).toContainText('Maintenance stopped');
});

for (const edited of [false, true]) {
  test(`maintenance invalidation preserves the document edit state: ${edited}`, async ({ page }) => {
    await scan(page);
    if (edited) {
      const memory = page.locator('#memory textarea');
      const lines = (await memory.inputValue()).split('\n');
      lines[1] = '11'.repeat(16);
      await memory.fill(lines.join('\n'));
    }
    await page.locator('#more-menu summary').click();
    await page.getByRole('button', { name: 'Factory format tag…', exact: true }).click();
    await page.getByRole('button', { name: 'Format card', exact: true }).click();
    await expect(page.locator('#status')).toContainText('Factory format verified');
    await expect(page.locator('#read')).toBeEnabled();
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    await page.locator('#slot-0 .bay-open').first().click();
    if (edited) {
      await expect(
        page.getByRole('dialog', { name: 'Replace the open document?', exact: true })
      ).toBeVisible();
      await page.getByRole('button', { name: 'Cancel', exact: true }).click();
    } else {
      await expect(page.locator('#studio-name')).toHaveText('Test Classic');
      await expect(page.locator('dialog[open]')).toHaveCount(0);
    }
  });
}

test('Classic scan uses one inspection and keeps legacy probe commands out of discovery', async ({
  page
}) => {
  const commands = [];
  page.on('request', (request) => {
    const match = request.url().match(/\/api\/command\/(\d+)/);
    if (match) {
      const command = Number(match[1]);
      if (command === 2000) {
        expect(request.postDataBuffer()?.length ?? 0).toBe(0);
      }
      commands.push(command);
    }
  });
  await scan(page, false);
  expect(commands).toEqual([2000, 2040]);
  await expect(page.locator('#identity-title')).toHaveText('Memory not read');
});

test('maintenance is scoped to Classic documents and failed NFC scans expose troubleshooting', async ({
  page
}) => {
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  await page.setViewportSize({ width: 360, height: 800 });
  await expect(page.locator('#studio-empty')).toBeVisible();
  await expect(page.locator('#more-menu')).toBeHidden();
  await expect(page.locator('#repair-tag')).toBeHidden();
  await expect(page.locator('#format-tag')).toBeHidden();
  await page.getByRole('button', { name: 'Scan', exact: true }).click();
  await page.route('**/api/command/2000?*', (route) => route.fulfill({ body: Buffer.from([0, 5]) }));
  await page.locator('#scan').click();
  await expect(page.locator('#scan-title')).toHaveText('Scan unavailable');
  await page.getByRole('button', { name: 'Troubleshoot MIFARE Classic…', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Check again', exact: true })).toBeEnabled();
  await expect(page.getByLabel('Known generation', { exact: true })).toBeVisible();
  await expect(page.getByLabel('Known card model', { exact: true })).toBeVisible();
  await page.getByLabel('Known generation', { exact: true }).selectOption('1');
  await page.getByLabel('Known card model', { exact: true }).selectOption('1');
  await page.getByRole('button', { name: 'Check again', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Close', exact: true })).toBeEnabled();
  await expect(page.locator('[data-changes]')).toHaveText('Identification unchanged');
  await page.getByRole('button', { name: 'Close', exact: true }).click();
  await expect(page.locator('#stop')).toBeHidden();
});

test('Factory format supports a fixed UID and preserves block 0 while erasing data', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  await request.post('/test/classic-block', { data: Buffer.from([1, ...Array(16).fill(42)]) });
  await scan(page, false);
  const writes = [];
  const operations = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?')) {
      const data = request.postDataBuffer();
      operations.push(data[0]);
      expect(data[1]).toBe(3);
      if (data[0] === 2) {
        writes.push(data[11]);
      }
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
  await expect(page.getByLabel('Known generation', { exact: true })).toBeHidden();
  await expect(page.locator('[data-changes]')).toHaveText('1 data block to erase · reset 1 sector');
  expect(writes).toEqual([]);
  await page.getByRole('button', { name: 'Format card', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Factory format verified');
  expect(writes).toEqual([1, 63]);
  expect(operations).toHaveLength(5);
  console.log(
    `Fixed-UID format: ${operations.length} maintenance exchanges, ${writes.length} verified writes`
  );
});

test('Repair option changes invalidate the preview and access-only repair preserves data', async ({
  page,
  request
}) => {
  await scan(page, false);
  await request.post('/test/classic-block', { data: Buffer.from([1, ...Array(16).fill(42)]) });
  for (const block of [11, 23]) {
    await request.post('/test/classic-block', {
      data: Buffer.from([block, ...Array(6).fill(255), 255, 7, 128, 0, ...Array(6).fill(255)])
    });
  }
  const writes = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?')) {
      const data = request.postDataBuffer();
      if (data[0] === 2) {
        writes.push(data[11]);
      }
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#repair-tag').click();
  await expect(page.getByRole('button', { name: 'Close', exact: true })).toBeEnabled();
  await expect(page.getByRole('heading', { name: 'No repair needed', exact: true })).toBeVisible();
  await expect(page.locator('.maintenance-result')).toContainText('For the selected options.');
  await expect(page.getByRole('button', { name: 'Cancel', exact: true })).toBeHidden();
  await expect(page.locator('.maintenance-preview dl')).toBeHidden();
  await page.screenshot({
    path: test.info().outputPath(`chameleon-no-repair-${test.info().project.name}.png`)
  });
  await page.getByLabel('Reset keys and access', { exact: true }).check();
  await expect(page.getByRole('button', { name: 'Check changes', exact: true })).toBeEnabled();
  await expect(page.locator('.maintenance-preview')).toBeHidden();
  await expect(page.locator('.maintenance-result')).toBeHidden();
  await expect(page.getByRole('button', { name: 'Cancel', exact: true })).toBeVisible();
  await page.getByLabel('Correct identification', { exact: true }).uncheck();
  await page.getByLabel('Sectors to reset', { exact: true }).fill('');
  await page.getByRole('button', { name: 'Check changes', exact: true }).click();
  await expect(page.locator('dialog[open]')).toContainText('Enter sectors to reset, or all.');
  await page.getByLabel('Sectors to reset', { exact: true }).fill('2,5');
  await page.getByRole('button', { name: 'Check again', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Apply repair', exact: true })).toBeEnabled();
  await expect(page.locator('[data-effect=data]')).toHaveText('Preserved');
  await expect(page.locator('[data-changes]')).toContainText('Reset sectors 2, 5');
  expect(writes).toEqual([]);
  await page.getByRole('button', { name: 'Apply repair', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Repair verified');
  await expect(page.locator('#toast')).toHaveText('Repair verified');
  await expect(page.locator('#toast')).toHaveClass('toast visible');
  expect(writes).toEqual([11, 23]);
});

test('Factory format reads a protected sector once with the successful key', async ({ page, request }) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  const secret = Buffer.from('123456789ABC', 'hex');
  const other = Buffer.from('AABBCCDDEEFF', 'hex');
  await request.post('/test/classic-block', {
    data: Buffer.concat([Buffer.from([7]), other, Buffer.from([127, 7, 136, 105]), secret])
  });
  await scan(page, false);
  await page.getByLabel('Read options', { exact: true }).click();
  await page.locator('#dictionary-edit').click();
  await page.locator('#dictionary').fill(secret.toString('hex'));
  await page.locator('#dictionary-close').click();
  const reads = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2027?')) {
      const data = request.postDataBuffer();
      if (data[1] === 1) {
        reads.push({ keyB: data[0] === 0x61, key: data.subarray(2, 8).toString('hex') });
      }
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
  expect(reads).toEqual([
    { keyB: false, key: secret.toString('hex') },
    { keyB: true, key: secret.toString('hex') }
  ]);
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
});

test('Repair reuses its initial block-0 read and previews identification changes', async ({
  page,
  request
}) => {
  await scan(page, false);
  await request.post('/test/classic-block', {
    data: Buffer.from([0, 1, 2, 3, 4, 5, 8, 4, 0, ...Array(8).fill(42)])
  });
  const operations = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?')) {
      operations.push(request.postDataBuffer()[0]);
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#repair-tag').click();
  await expect(page.getByRole('button', { name: 'Apply repair', exact: true })).toBeEnabled();
  expect(operations).toEqual([0, 1]);
  await expect(page.locator('[data-changes]')).toHaveText('Correct identification (block 0)');
  await expect(page.getByLabel('Replacement UID (optional)', { exact: true })).toBeHidden();
  await page.getByRole('button', { name: 'Apply repair', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Repair verified');
  expect(operations).toEqual([0, 1, 2, 0]);
});

test('Factory format accepts a missing key after inspection without generation overrides', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  const secret = Buffer.from('D1950B37E862', 'hex');
  await request.post('/test/classic-block', {
    data: Buffer.concat([Buffer.from([3]), secret, Buffer.from([255, 7, 128, 105]), secret])
  });
  await scan(page, false);
  const writes = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?')) {
      const data = request.postDataBuffer();
      if (data[0] === 2) {
        writes.push(data[11]);
      }
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await expect(page.getByRole('button', { name: 'Check again', exact: true })).toBeEnabled();
  await expect(page.locator('dialog[open]')).toContainText('No usable sector-0 key');
  await page.getByLabel('Known target key', { exact: true }).fill(secret.toString('hex'));
  await page.getByRole('button', { name: 'Check again', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
  expect(writes).toEqual([]);
  await page.getByRole('button', { name: 'Format card', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Factory format verified');
  expect(writes).toEqual([3, 63]);
  expect(writes).not.toContain(0);
});

test('Factory format tries a denied preferred key only once before using Key B', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  await request.post('/test/classic-block', {
    data: Buffer.from([3, ...Array(6).fill(255), 127, 7, 136, 105, ...Array(6).fill(255)])
  });
  await scan(page, false);
  const attempts = [];
  await page.route('**/api/command/2039?*', async (route) => {
    const data = route.request().postDataBuffer();
    if (data[0] === 1 && data[11] === 3) {
      attempts.push(data[12]);
      if (data[12] === 1) {
        await route.fulfill({ body: Buffer.from([0, 0x75]) });
        return;
      }
    }
    await route.continue();
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
  expect(attempts).toEqual([1, 2]);
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
});

test('cancelling maintenance while changed options release the reader closes the dialog after cleanup', async ({
  page,
  request
}) => {
  await scan(page, false);
  await page.locator('#more-menu summary').click();
  await page.locator('#repair-tag').click();
  await expect(page.getByRole('button', { name: 'Close', exact: true })).toBeEnabled();
  let release;
  let started;
  const pending = new Promise((resolve) => {
    release = resolve;
  });
  const ending = new Promise((resolve) => {
    started = resolve;
  });
  await page.route('**/api/end?*', async (route) => {
    started();
    await pending;
    await route.continue();
  });
  await page.getByLabel('Reset keys and access', { exact: true }).check();
  await ending;
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  release();
  await expect(page.locator('dialog[open]')).toHaveCount(0);
  await expect(page.locator('#read')).toBeEnabled();
  expect((await (await request.get('/api/device')).json()).busy).toBe(false);
});

for (const kind of ['classic', 'fixed-classic']) {
  test(`Factory format skips every write when the card is already formatted: ${kind}`, async ({
    page,
    request
  }) => {
    await page.setViewportSize({ width: 360, height: 820 });
    await request.post('/test/reset', { data: kind });
    await request.post('/test/classic-block', {
      data: Buffer.from([63, ...Array(6).fill(255), 255, 7, 128, 188, ...Array(6).fill(255)])
    });
    await scan(page, false);
    const maintenance = [];
    const sectors = [];
    page.on('request', (request) => {
      if (request.url().includes('/api/command/2039?')) {
        maintenance.push(request.postDataBuffer()[0]);
      }
      if (request.url().includes('/api/command/2027?')) {
        sectors.push(request.postDataBuffer()[1]);
      }
    });
    await page.locator('#more-menu summary').click();
    await page.locator('#format-tag').click();
    await expect(page.getByRole('button', { name: 'Close', exact: true })).toBeEnabled();
    await expect(page.getByRole('heading', { name: 'Already formatted', exact: true })).toBeVisible();
    await expect(page.locator('.maintenance-result')).toContainText('The card has not been modified.');
    await expect(page.getByRole('button', { name: 'Cancel', exact: true })).toBeHidden();
    await expect(page.locator('.maintenance-preview dl')).toBeHidden();
    await page.screenshot({
      path: test.info().outputPath(`chameleon-formatted-${kind}-${test.info().project.name}.png`)
    });
    await page.getByText('Details', { exact: true }).click();
    await expect(page.locator('.maintenance-preview dl')).toBeVisible();
    await expect(page.locator('[data-changes]')).toHaveText('Card already formatted · no writes needed');
    if (kind === 'fixed-classic') {
      expect(sectors).toEqual(Array.from({ length: 16 }, (_, sector) => sector));
      expect(maintenance).toEqual([]);
    }
    await page.getByRole('button', { name: 'Close', exact: true }).click();
    await expect(page.locator('#status')).toContainText('Already formatted · no writes sent');
    expect(maintenance).not.toContain(1);
    expect(maintenance).not.toContain(2);
    await expect(page.locator('#read')).toBeEnabled();
  });
}

for (const different of ['A', 'B']) {
  test(`Factory format does not mistake a hidden non-default Key ${different} for a formatted sector`, async ({
    page,
    request
  }) => {
    await request.post('/test/reset', { data: 'fixed-classic' });
    const secret = Buffer.from('A0A1A2A3A4A5', 'hex');
    const defaultKey = Buffer.alloc(6, 255);
    const keyA = different === 'A' ? secret : defaultKey;
    const keyB = different === 'B' ? secret : defaultKey;
    await request.post('/test/classic-block', {
      data: Buffer.concat([Buffer.from([3]), keyA, Buffer.from([255, 7, 128, 105]), keyB])
    });
    await request.post('/test/classic-block', {
      data: Buffer.from([63, ...Array(6).fill(255), 255, 7, 128, 188, ...Array(6).fill(255)])
    });
    await scan(page, false);
    const writes = [];
    page.on('request', (request) => {
      if (request.url().includes('/api/command/2039?')) {
        const data = request.postDataBuffer();
        if (data[0] === 2) {
          writes.push(data[11]);
        }
      }
    });
    await page.locator('#more-menu summary').click();
    await page.locator('#format-tag').click();
    await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
    await expect(page.locator('[data-changes]')).toHaveText('Data already zero · reset 1 sector');
    expect(writes).toEqual([]);
    await page.getByRole('button', { name: 'Format card', exact: true }).click();
    await expect(page.locator('#status')).toContainText('Factory format verified');
    expect(writes).toEqual([3]);
    await page.locator('#more-menu summary').click();
    await page.locator('#format-tag').click();
    await expect(page.getByRole('button', { name: 'Close', exact: true })).toBeEnabled();
    await page.getByRole('button', { name: 'Close', exact: true }).click();
    expect(writes).toEqual([3]);
  });
}

test('Factory format tries Key B after a data read NAK with Key A', async ({ page, request }) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  await request.post('/test/classic-block', {
    data: Buffer.from([1, ...Array(16).fill(42)])
  });
  // Group 1 is readable/writable with B only; B can also reset this trailer.
  await request.post('/test/classic-block', {
    data: Buffer.from([3, ...Array(6).fill(255), 0x5f, 0x05, 0xaa, 0x69, ...Array(6).fill(255)])
  });
  await request.post('/test/classic-block', {
    data: Buffer.from([63, ...Array(6).fill(255), 255, 7, 128, 188, ...Array(6).fill(255)])
  });
  await scan(page, false);
  const reads = [];
  const writes = [];
  await page.route('**/api/command/2027?*', async (route) => {
    const response = await route.fetch();
    const body = await response.body();
    const data = route.request().postDataBuffer();
    if (data[1] === 0 && data[0] === 0x60) {
      body[3] &= ~2;
    }
    await route.fulfill({ response, body });
  });
  await page.route('**/api/command/2039?*', async (route) => {
    const data = route.request().postDataBuffer();
    if (data[11] === 1 && data[0] === 0) {
      reads.push(data[12]);
      if (data[12] === 1) {
        await route.fulfill({ body: Buffer.from([0, 2]) });
        return;
      }
    }
    if ([1, 3].includes(data[11]) && data[0] !== 0) {
      expect(data[12]).toBe(2);
    }
    if (data[0] === 2) {
      writes.push(data[11]);
    }
    await route.continue();
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await expect(page.getByRole('button', { name: 'Format card', exact: true })).toBeEnabled();
  expect(reads).toEqual([1, 2]);
  expect(writes).toEqual([]);
  await page.getByRole('button', { name: 'Format card', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Factory format verified');
  expect(writes).toEqual([1, 3]);
});

test('Factory format completes a partial sector read with checked block reads', async ({ page, request }) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  await request.post('/test/classic-block', {
    data: Buffer.from([63, ...Array(6).fill(255), 255, 7, 128, 188, ...Array(6).fill(255)])
  });
  await scan(page, false);
  const fallback = [];
  await page.route('**/api/command/2027?*', async (route) => {
    const response = await route.fetch();
    const body = await response.body();
    if (route.request().postDataBuffer()[1] === 5) {
      body[2] = 0;
      body[3] = 3;
    }
    await route.fulfill({ response, body });
  });
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?')) {
      const data = request.postDataBuffer();
      expect(data[0]).toBe(0);
      fallback.push(data[11]);
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await expect(page.getByRole('button', { name: 'Close', exact: true })).toBeEnabled();
  expect(fallback).toEqual([22, 23]);
  await page.getByRole('button', { name: 'Close', exact: true }).click();
  await expect(page.locator('#status')).toContainText('Already formatted');
});

test('cancelling a grouped maintenance read retains no pending confirmation and releases the reader', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'fixed-classic' });
  await scan(page, false);
  let started;
  let release;
  const reached = new Promise((resolve) => {
    started = resolve;
  });
  const pending = new Promise((resolve) => {
    release = resolve;
  });
  const writes = [];
  await page.route('**/api/command/2027?*', async (route) => {
    if (route.request().postDataBuffer()[1] === 15) {
      started();
      await pending;
    }
    await route.continue();
  });
  page.on('request', (request) => {
    if (request.url().includes('/api/command/2039?') && request.postDataBuffer()[0] === 2) {
      writes.push(request.postDataBuffer()[11]);
    }
  });
  await page.locator('#more-menu summary').click();
  await page.locator('#format-tag').click();
  await reached;
  await page.getByRole('button', { name: 'Cancel', exact: true }).click();
  release();
  await expect(page.locator('dialog[open]')).toHaveCount(0);
  await expect(page.locator('#read')).toBeEnabled();
  await expect(page.locator('#status')).toContainText('Maintenance stopped');
  expect(writes).toEqual([]);
  expect((await (await request.get('/api/device')).json()).busy).toBe(false);
});

test('exports stay silent on success and report download failures', async ({ page }) => {
  await scan(page);
  for (const button of ['#export', '#export-json', '#export-mct']) {
    await exported(page, button);
    await expect(page.locator('#toast')).not.toHaveClass(/visible/);
  }

  await page.evaluate(() => {
    URL.createObjectURL = () => {
      throw new Error('Download unavailable');
    };
  });
  for (const id of ['export', 'export-document', 'export-mct']) {
    if (!(await page.locator('#' + id).isVisible())) {
      await page.locator('#export-menu summary').click();
    }
    await page.locator('#' + id).click();
    await expect(page.locator('#toast')).toHaveText('Download unavailable');
    await expect(page.locator('#toast')).toHaveClass('toast visible error');
    await expect(page.locator('#scan')).toBeEnabled();
  }
});

test('dictionary import and export feedback remains visible inside its dialog', async ({ page }) => {
  await scan(page, false);
  await page.locator('#read-menu summary').click();
  await page.locator('#dictionary-edit').click();
  const status = page.locator('#dictionary-status');
  const file = page.locator('#dictionary-file');
  const valid = { name: 'valid.keys', mimeType: 'text/plain', buffer: Buffer.from('FFFFFFFFFFFF') };
  await file.setInputFiles(valid);
  await expect(status).toContainText('Keys imported.');
  await expect(page.locator('#dictionary')).toHaveValue('FFFFFFFFFFFF');

  const download = page.waitForEvent('download');
  await page.locator('#dictionary-export').click();
  await download;
  await expect(status).toContainText('Keys imported.');
  await expect(page.locator('#toast')).not.toHaveClass(/visible/);

  await file.setInputFiles({ name: 'invalid.keys', mimeType: 'text/plain', buffer: Buffer.from('invalid') });
  await expect(status).toHaveText('Use one 12-character hexadecimal key per line');
  await expect(status).toBeVisible();
  await expect(page.locator('#dictionary')).toHaveValue('FFFFFFFFFFFF');

  await page.evaluate(() => {
    Storage.prototype.setItem = () => {
      throw new Error('Storage unavailable');
    };
    URL.createObjectURL = () => {
      throw new Error('Download unavailable');
    };
  });
  await file.setInputFiles(valid);
  await expect(status).toContainText('Keys imported. Browser storage unavailable.');
  await page.locator('#dictionary-export').click();
  await expect(status).toHaveText('Download unavailable');
  await expect(status).toBeVisible();
  await expect(page.locator('#dictionary-dialog')).toBeVisible();
  await expect(page.locator('#toast')).not.toHaveClass(/visible/);
});

for (const action of ['rename', 'clear']) {
  test(`slot ${action} confirms success and never confirms a cancellation or failed write`, async ({
    page
  }) => {
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    const button = page.getByRole('button', {
      name: `${action === 'rename' ? 'Rename' : 'Clear'} slot 1 hf`,
      exact: true
    });
    await button.click();
    await page.getByRole('dialog').getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(page.locator('#toast')).not.toHaveClass(/visible/);

    await page.route(
      `**/api/slots/${action === 'rename' ? 'nick' : 'clear'}?*`,
      (route) => route.fulfill({ status: 503 }),
      { times: 1 }
    );
    await button.click();
    if (action === 'rename') {
      await page.getByRole('dialog').getByRole('textbox').fill('Renamed');
    }
    await page
      .getByRole('dialog')
      .getByRole('button', { name: action === 'rename' ? 'Rename' : 'Clear', exact: true })
      .click();
    await expect(page.locator('#toast')).toHaveClass('toast visible error');
    await expect(page.locator('#slot-0 .bay-open').first()).toContainText('Test Classic');
    await expect(page.locator('#scan')).toBeEnabled();

    await button.click();
    if (action === 'rename') {
      await page.getByRole('dialog').getByRole('textbox').fill('Renamed');
    }
    await page
      .getByRole('dialog')
      .getByRole('button', { name: action === 'rename' ? 'Rename' : 'Clear', exact: true })
      .click();
    await expect(page.locator('#toast')).toHaveText(
      `Slot 1 NFC ${action === 'rename' ? 'renamed' : 'cleared'}`
    );
    await expect(page.locator('#toast')).toHaveClass('toast visible');
    if (action === 'rename') {
      await expect(page.locator('#slot-0 .bay-open').first()).toContainText('Renamed');
    } else {
      await expect(button).toHaveCount(0);
    }
  });
}

test('ST25TB confirms verified writes and matching tags but not cancelled writes', async ({
  page,
  request
}) => {
  await request.post('/test/reset', { data: 'st25tb' });
  await scan(page);
  const write = page.getByRole('button', { name: 'Write changes', exact: true });
  await expect(page.locator('#save-menu')).not.toHaveAttribute('open');
  await expect(write).toBeVisible();
  await expect(write).toBeEnabled();
  await page.locator('#save-menu summary').click();
  await expect(page.locator('#save-menu').getByRole('button')).toHaveText([
    'Save to slot…',
    'Slot & emulate'
  ]);
  await expect(page.locator('#copy-tag-divider')).toBeHidden();
  await page.locator('#save-menu summary').click();
  const blocks = new Map();
  await page.route('**/api/command/2311?*', async (route) => {
    const payload = route.request().postDataBuffer();
    const block = payload[8];
    const bytes = payload.subarray(13);
    blocks.set(block, bytes);
    await route.fulfill({ body: Buffer.from([0, 0, ...bytes]) });
  });
  await page.route('**/api/command/2306?*', async (route) => {
    const block = route.request().postDataBuffer()[8];
    await route.fulfill({ body: Buffer.from([0, 0, ...blocks.get(block)]) });
  });
  await page.route('**/api/command/2307?*', async (route) => {
    const response = await route.fetch();
    const body = await response.body();
    for (const [block, bytes] of blocks) {
      body.set(bytes, 28 + block * 6);
    }
    await route.fulfill({ response, body });
  });
  const memory = page.locator('#memory textarea');
  const lines = (await memory.inputValue()).split('\n');
  lines[1] = '44332211';
  await memory.fill('invalid');
  await expect(write).toBeDisabled();
  await memory.fill(lines.join('\n'));
  await expect(write).toBeEnabled();
  await write.click();
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.locator('#scan')).toBeEnabled();
  await expect(page.locator('#toast')).not.toHaveClass(/visible/);
  expect(blocks.size).toBe(0);

  await write.click();
  await page.getByRole('dialog').getByRole('button', { name: 'Write changes', exact: true }).click();
  await expect(page.locator('#toast')).toHaveText('Changes written and verified');
  await expect(page.locator('#toast')).toHaveClass('toast visible');
  await expect(page.locator('#scan')).toBeEnabled();
  expect(blocks.size).toBe(1);

  await write.click();
  await expect(page.locator('#toast')).toHaveText('Tag already matches');
  await expect(page.locator('#toast')).toHaveClass('toast visible');
  expect(blocks.size).toBe(1);
});

test('Classic write waits for cleanup and reports a cleanup failure instead of success', async ({
  page,
  request
}) => {
  await scan(page);
  const memory = page.locator('#memory textarea');
  const lines = (await memory.inputValue()).split('\n');
  lines[1] = '11'.repeat(16);
  await memory.fill(lines.join('\n'));
  let release;
  let cleaning = false;
  const cleanup = new Promise((resolve) => {
    release = resolve;
  });
  await page.route('**/api/end?*', async (route) => {
    await route.fetch();
    cleaning = true;
    await cleanup;
    await route.fulfill({ status: 503 });
  });
  await page.locator('#write-changes').click();
  await page.getByRole('dialog').getByRole('button', { name: 'Write changes', exact: true }).click();
  try {
    await expect.poll(() => cleaning).toBe(true);
    await expect(page.locator('#toast')).not.toHaveClass(/visible/);
  } finally {
    release();
  }
  await expect(page.locator('#toast')).toHaveClass('toast visible error');
  await expect(page.locator('#toast')).toContainText('1/1 blocks written and verified');
  expect((await (await request.get('/api/device')).json()).busy).toBe(false);
});

test('DESFire directory exposes captured and denied files with keyboard-accessible details', async ({
  page
}) => {
  const capture = JSON.parse(
    await readFile(new URL('../../../../resource/tools/tests/desfire-public.json', import.meta.url), 'utf8')
  );
  await importFile(page, 'desfire.json', JSON.stringify(capture));
  const directory = page.locator('#memory');
  await expect(directory.locator('.desfire-overview dd')).toHaveText(['1', '3', '1536 B']);
  const application = directory.locator('.desfire-application');
  const applicationTitle = application.locator(':scope > summary');
  await expect(applicationTitle).toContainText('335848');
  await applicationTitle.focus();
  await page.keyboard.press('Enter');
  await expect(application.locator('.desfire-files')).toBeHidden();
  await page.keyboard.press('Enter');
  const files = application.locator('.desfire-file');
  await expect(files).toHaveCount(3);
  await files.nth(0).locator('summary').focus();
  await page.keyboard.press('Enter');
  await expect(files.nth(0).locator('pre')).toBeVisible();
  await expect(files.nth(0).locator('pre')).toContainText('0000  DB9629467558AB01BFD229166B23806B');
  await expect(files.nth(1).locator('summary')).toContainText('Access denied');
  await files.nth(1).locator('summary').click();
  await expect(files.nth(1)).toContainText('No public data captured for this file.');
  await expect(files.nth(1).locator('pre')).toHaveCount(0);
  await directory.getByText('Card details', { exact: true }).click();
  await expect(directory.locator('.desfire-card-details')).toContainText('067577810080');

  await page.setViewportSize({ width: 390, height: 844 });
  await expect(files.nth(0).locator('pre')).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  expect(JSON.parse(await exported(page, '#export-json'))).toEqual(capture);
});

for (const [state, title] of [
  [0, 'No applications found'],
  [1, 'Application directory · Access denied'],
  [2, 'Application directory · Unsupported'],
  [3, 'Application directory · Not read'],
  [null, 'Applications not read yet']
]) {
  test(`DESFire empty state: ${title}`, async ({ page }) => {
    const capture = JSON.parse(
      await readFile(new URL('../../../../resource/tools/tests/desfire-public.json', import.meta.url), 'utf8')
    );
    capture.memory = [68, 70, 68, 49, 1, 0];
    if (state === null) {
      capture.metadata = [68, 70, 85, 49, ...capture.metadata.slice(5, 9 + capture.metadata[8])];
      capture.memory = [];
      capture.complete = false;
    } else {
      capture.metadata[capture.metadata.length - 1] = state;
    }
    await importFile(page, 'desfire.json', JSON.stringify(capture));
    await expect(page.getByRole('heading', { name: title, exact: true })).toBeVisible();
    await expect(page.locator('.desfire-application')).toHaveCount(0);
    await page.setViewportSize({ width: 390, height: 844 });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  });
}
