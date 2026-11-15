import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { encodeNfcvSlot } from '../../src/documents/nfcv-slot.mjs';
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

function withSlixCrc(data) {
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

async function createSlixL(page) {
  await page.locator('#studio-new').click();
  await page.locator('#create-search').fill('SLIX-L');
  await page.locator('[data-create="1500"]').click();
  await expect(page.locator('#studio-family')).toHaveText('SLIX-L');
}

function slixInventory(uid) {
  return withSlixCrc([0, 12, ...uid.toReversed()]);
}

function slixDump(uid, blocks = 8) {
  const reply = new Uint8Array(2 + 24 + blocks * 6);
  reply.set([0, 0, 1, 3, 0, 0, ...uid, 0, blocks, 4]);
  for (let index = 0; index < blocks; index++) {
    reply.set([1, 0, uid[7], 0, 0, index], 26 + index * 6);
  }
  return reply;
}

async function mockSlix(page, commands) {
  await page.route(/\/api\/command\/(2300|2301|2308|2312)\?/, async (route) => {
    const command = Number(new URL(route.request().url()).pathname.split('/').at(-1));
    const payload = [...(route.request().postDataBuffer() ?? [])];
    const reply = commands(command, payload);
    await route.fulfill({ body: Buffer.from(reply) });
  });
}

async function readTonie(page) {
  await page.locator('#read-menu summary').click();
  await page.locator('#read-tonie').click();
}

test('SLIX-L privacy password is visible and retries in the scan drawer until cancelled', async ({
  page
}) => {
  await page.goto('/');
  await expect(page.locator('[data-view="scan"]').getByRole('button', { name: /SLIX-L/ })).toHaveCount(0);
  await expect(page.locator('[data-view="scan"]').getByRole('button', { name: /tonie/i })).toHaveCount(0);
  await page.getByRole('radio', { name: 'RFID 125 kHz' }).click();
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  await createSlixL(page);
  const protectedRead = page.getByRole('button', { name: 'Read with privacy password…', exact: true });
  const tonieRead = page.getByRole('button', { name: 'Read tonie…', exact: true });
  const input = page.getByRole('textbox', { name: 'SLIX-L privacy password' });
  for (const width of [320, 1280]) {
    await page.setViewportSize({ width, height: 900 });
    await page.locator('#read-menu summary').focus();
    await page.keyboard.press('Enter');
    await expect(protectedRead).toBeVisible();
    await expect(tonieRead).toBeVisible();
    expect(await protectedRead.evaluate((button) => button.getBoundingClientRect().right <= innerWidth)).toBe(
      true
    );
    await expect(protectedRead).toBeEnabled();
    await expect(tonieRead).toBeEnabled();
    await page.screenshot({
      path: test.info().outputPath(`chameleon-slix-${width}-${test.info().project.name}.png`)
    });
    await protectedRead.focus();
    await page.keyboard.press('Enter');
    await expect(input).toHaveJSProperty('type', 'text');
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
  await mockSlix(page, (command, payload) => {
    expect(command).toBe(2312);
    expect(payload).toEqual([0x11, 0x22, 0x33, 0x44]);
    attempts++;
    return [0, attempts === 1 ? 1 : 6];
  });

  await page.getByRole('button', { name: 'Read tag', exact: true }).click();
  await expect(page.locator('#scan-dialog')).toBeVisible();
  await expect(page.locator('#scan-title')).toHaveText('Scanning');
  await expect.poll(() => attempts).toBeGreaterThanOrEqual(3);
  await expect(page.locator('#scan-result')).toHaveText('');
  await page.locator('#cancel-scan').click();
  await expect(page.locator('#scan-dialog')).toBeHidden();
  await expect(page.locator('#scan')).toBeEnabled();
  await expect(memory).toHaveValue(original);
});

const toniePassword = [0x5b, 0x6e, 0xfd, 0x7f];
const defaultPassword = [0x0f, 0x0f, 0x0f, 0x0f];
const slixUid = [0xe0, 4, 3, 0x50, 1, 2, 3, 4];

for (const [scenario, steps] of [
  [
    'the default password unlocks the tag',
    [
      [2312, toniePassword, [0, 6]],
      [2312, defaultPassword, slixInventory(slixUid)],
      [2308, [...slixUid, 0, 8, 4, ...defaultPassword], slixDump(slixUid)]
    ]
  ],
  [
    'the tag is open and an interrupted dump is retried',
    [
      [2312, toniePassword, [0, 1]],
      [2312, toniePassword, [0, 1]],
      [2312, toniePassword, [0, 1]],
      [2300, [], slixInventory(slixUid)],
      [2308, [...slixUid, 0, 8, 4], [0, 6]],
      [2312, toniePassword, slixInventory(slixUid)],
      [2308, [...slixUid, 0, 8, 4, ...toniePassword], slixDump(slixUid)]
    ]
  ]
]) {
  test(`Read Tonie falls back when ${scenario}`, async ({ page }) => {
    await createSlixL(page);
    let count = 0;
    await mockSlix(page, (command, payload) => {
      const [expectedCommand, expectedPayload, reply] = steps[count++];
      expect(command).toBe(expectedCommand);
      expect(payload).toEqual(expectedPayload);
      return reply;
    });

    await readTonie(page);
    await expect(page.getByRole('textbox', { name: 'SLIX-L privacy password' })).toHaveCount(0);
    await expect(page.locator('#status')).toHaveText('Read complete');
    await expect(page.locator('#scan-dialog')).toBeHidden();
    await expect(page.locator('#memory-status')).toHaveText('8/8 blocks known');
    await expect(page.locator('#emulate')).toBeEnabled();
    const draft = () => page.evaluate((key) => JSON.parse(localStorage.getItem(key))?.document, draftKey);
    await expect
      .poll(async () => (await draft())?.emulator?.secrets.slice(0, 4))
      .toEqual([0x7f, 0xfd, 0x6e, 0x5b]);
    const slot = encodeNfcvSlot(await draft());
    expect(slot[11] & 8).toBe(0);
    expect(slot.slice(20, 24)).toEqual(toniePassword);
    expect(count).toBe(steps.length);
  });
}

test('NFC-V reread adopts the presented tag before dumping memory', async ({ page }) => {
  await createSlixL(page);
  const uid2 = [...slixUid.slice(0, 4), 0x11, 0x22, 0x33, 0x44];
  const uid3 = [0xe0, 4, 1, 0x50, 0x55, 0x66, 0x77, 0x88];
  let activeUid = slixUid;
  let dumpFails = false;
  await mockSlix(page, (command, payload) => {
    if (command === 2312 || command === 2300) {
      return slixInventory(activeUid);
    }
    const blocks = activeUid === uid3 ? 28 : 8;
    if (command === 2301) {
      expect(payload).toEqual(uid3);
      return withSlixCrc([0, 4, ...uid3.toReversed(), blocks - 1, 3]);
    }
    expect(command).toBe(2308);
    expect(payload.slice(0, 11)).toEqual([...activeUid, 0, blocks, 4]);
    if (dumpFails) {
      return [0, 1];
    }
    const reply = slixDump(activeUid, blocks);
    if (activeUid === slixUid) {
      reply[4] = 1;
      reply.fill(0, reply.length - 6);
    }
    return reply;
  });

  await readTonie(page);
  await expect(page.locator('#card')).toContainText('01020304');
  await expect(page.locator('#memory-status')).toHaveText('7/8 blocks known');
  await expect(page.locator('#hf-defaults')).toBeVisible();
  await expect(page.locator('#save-slot')).toBeDisabled();

  const confirmRead = page.getByRole('dialog').getByRole('button', { name: 'Read', exact: true });
  activeUid = uid2;
  await readTonie(page);
  await confirmRead.click();
  await expect(page.locator('#status')).toHaveText('Read complete');
  await expect(page.locator('#card')).toContainText('11223344');

  activeUid = uid3;
  dumpFails = true;
  await page.locator('#read').click();
  await confirmRead.click();
  await expect(page.locator('#status')).toHaveText('Card not found');
  await expect(page.locator('#card')).toContainText('55667788');
  await expect(page.locator('#studio-family')).toHaveText('SLIX');
  await expect(page.locator('#memory-status')).toHaveText('0/28 blocks known');

  dumpFails = false;
  await page.locator('#read').click();
  await expect(page.locator('#status')).toHaveText('Read complete');
  await expect(page.locator('#memory-status')).toHaveText('28/28 blocks known');
});
