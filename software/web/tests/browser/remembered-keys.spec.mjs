import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';

const cachePrefix = 'chameleon-classic-keys-v1:';
let errors;

test.beforeEach(async ({ page, request }) => {
  await request.post('/test/reset');
  errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route(RESOURCE_BASE + '**', async (route) => {
    const path = route.request().url().slice(RESOURCE_BASE.length);
    await route.fulfill({
      body: await readFile(new URL('../../../../.cache/web/cdn/' + path, import.meta.url)),
      contentType: 'application/octet-stream',
      headers: { 'Access-Control-Allow-Origin': '*' }
    });
  });
});

test.afterEach(() => expect(errors).toEqual([]));

async function readTag(page) {
  await page.goto('/');
  await expect(page.locator('#scan')).toBeEnabled();
  await page.locator('#scan').click();
  await expect(page.locator('#status')).toHaveText('Tag identified · Memory not read');
  await page.locator('#read').click();
  await expect(page.locator('#status')).toHaveText('Read complete');
  await expect(page.locator('#stop')).toBeHidden();
}

async function rereadTag(page) {
  const started = page.waitForRequest((request) => new URL(request.url()).pathname === '/api/command/2000');
  await page.locator('#read').click();
  await page
    .getByRole('dialog', { name: 'Read fresh memory?', exact: true })
    .getByRole('button', { name: 'Recover', exact: true })
    .click();
  await started;
  await expect(page.locator('#stop')).toBeHidden();
  await expect(page.locator('#status')).toHaveText('Read complete');
}

test('Device shows remembered keys and clearing keeps the dictionary and document', async ({ page }) => {
  await page.goto('/#device');
  const clear = page.getByRole('button', { name: 'Clear remembered tag keys', exact: true });
  const status = page.locator('#remembered-keys-status');
  await expect(status).toHaveText('No tags yet · Saved in this browser');
  await expect(clear).toBeDisabled();

  await readTag(page);
  await page.evaluate(() => localStorage.setItem('chameleon-dictionary-v1', 'A0A1A2A3A4A5'));
  await expect
    .poll(() =>
      page.evaluate(
        () => JSON.parse(localStorage.getItem('chameleon-studio-v1'))?.document.memory.filter(Boolean).length
      )
    )
    .toBe(64);
  const draft = await page.evaluate(() => localStorage.getItem('chameleon-studio-v1'));
  await page.locator('[data-view-target="device"]').click();
  await expect(status).toHaveText('1 tag · Saved in this browser');
  await expect(clear).toBeEnabled();
  await clear.click();
  const confirmation = page.getByRole('dialog', { name: 'Clear remembered tag keys?', exact: true });
  await expect(confirmation).toContainText('Your personal dictionary and documents will be kept.');
  await expect(confirmation.getByRole('button', { name: 'Cancel', exact: true })).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(confirmation).toHaveCount(0);
  await expect(clear).toBeFocused();
  await expect(status).toHaveText('1 tag · Saved in this browser');
  await clear.click();
  await confirmation.getByRole('button', { name: 'Clear keys', exact: true }).click();
  await expect(status).toHaveText('No tags yet · Saved in this browser');
  await expect(clear).toBeDisabled();
  expect(await page.evaluate(() => localStorage.getItem('chameleon-dictionary-v1'))).toBe('A0A1A2A3A4A5');
  expect(await page.evaluate(() => JSON.parse(localStorage.getItem('chameleon-studio-v1')).document)).toEqual(
    JSON.parse(draft).document
  );
  expect(
    await page.evaluate(
      (prefix) => Object.keys(localStorage).filter((key) => key.startsWith(prefix)),
      cachePrefix
    )
  ).toEqual([]);
});

test('a reloaded session uses remembered keys and locks clearing until reading ends', async ({ page }) => {
  await readTag(page);
  await page.reload();
  await expect(page.locator('#memory-status')).toHaveText('64/64 blocks known');
  let release;
  let observed;
  const waiting = new Promise((resolve) => {
    observed = resolve;
  });
  let commands = [];
  await page.route('**/api/command/**', async (route) => {
    const command = Number(new URL(route.request().url()).pathname.split('/').at(-1));
    commands.push(command);
    if (command === 2042 && !release) {
      observed();
      await new Promise((resolve) => {
        release = resolve;
      });
    }
    await route.continue();
  });
  try {
    await page.locator('#read').click();
    await page
      .getByRole('dialog', { name: 'Read fresh memory?', exact: true })
      .getByRole('button', { name: 'Recover', exact: true })
      .click();
    await waiting;
    await page.locator('[data-view-target="device"]').click();
    await expect(page.locator('#remembered-keys-clear')).toBeDisabled();
  } finally {
    release?.();
  }
  await expect(page.locator('#remembered-keys-clear')).toBeEnabled();
  expect(commands).not.toContain(2015);
  expect(commands).not.toContain(2002);
  expect(commands.filter((command) => command === 2042)).toHaveLength(3);
});

for (const [sectors, keyB] of [
  [[0], false],
  [[7], false],
  [Array.from({ length: 16 }, (_, i) => i), false],
  [[7], true]
]) {
  test(`a first read followed by changed key ${keyB ? 'B' : 'A'} is recovered: sectors ${sectors.length === 16 ? 'all' : sectors}`, async ({
    page,
    request
  }) => {
    if (keyB) {
      for (let sector = 0; sector < 16; sector++) {
        await request.post('/test/classic-block', {
          data: Buffer.from([
            sector * 4 + 3,
            ...Array(6).fill(255),
            0x7f,
            7,
            0x88,
            0x69,
            ...Array(6).fill(255)
          ])
        });
      }
    }
    await readTag(page);
    const key = [0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5];
    for (const sector of sectors) {
      await request.post('/test/classic-block', {
        data: Buffer.from([
          sector * 4 + 3,
          ...(keyB
            ? [...Array(6).fill(255), 0x7f, 7, 0x88, 0x69, ...key]
            : [...key, 255, 7, 128, 105, ...Array(6).fill(255)])
        ])
      });
    }
    let captures = 0;
    const rejected = [];
    page.on('request', (r) => {
      const command = Number(new URL(r.url()).pathname.split('/').at(-1));
      if (command === 2023) {
        captures++;
      }
      if (command === 2042) {
        const data = r.postDataBuffer();
        const start = 4 + data[2];
        for (let index = start; index < data.length; index += 8) {
          const sector = data[1] ? data[index] : Math.floor(data[index] / 4);
          if (
            sectors.includes(sector) &&
            data[index + 1] === (keyB ? 0x61 : 0x60) &&
            data.subarray(index + 2, index + 8).every((b) => b === 255)
          ) {
            rejected.push(sector);
          }
        }
      }
    });
    await rereadTag(page);
    const keys = await page.evaluate(
      (prefix) =>
        JSON.parse(localStorage.getItem(Object.keys(localStorage).find((id) => id.startsWith(prefix)))).keys,
      cachePrefix
    );
    for (const sector of sectors) {
      expect(keys[sector][keyB ? 'key_b' : 'key_a']).toEqual(key);
      expect(rejected.filter((value) => value === sector)).toHaveLength(1);
    }
    expect(captures).toBe(0);
    await rereadTag(page);
    expect(rejected).toHaveLength(sectors.length);
  });
}

test('remembered keys stay accessible without a device and fit narrow screens', async ({
  page
}, testInfo) => {
  await page.addInitScript(() => {
    localStorage.setItem(
      'chameleon-classic-keys-v1:01020304:8:0004:64',
      JSON.stringify({
        version: 1,
        keys: Array.from({ length: 16 }, () => ({ key_a: Array(6).fill(255), key_b: null, origin_b: null }))
      })
    );
  });
  await page.route('**/api/device', (route) => route.fulfill({ status: 503 }));
  for (const width of [320, 390, 1280]) {
    await page.setViewportSize({ width, height: 900 });
    await page.goto('/#device');
    await expect(page.locator('#connection-title')).toHaveText('Ultra not reachable');
    await expect(page.locator('#device-settings')).toBeHidden();
    await expect(page.locator('#remembered-keys-status')).toHaveText('1 tag · Saved in this browser');
    const clear = page.locator('#remembered-keys-clear');
    await expect(clear).toBeVisible();
    await expect(clear).toBeEnabled();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const box = await clear.boundingBox();
    expect(box.width).toBeGreaterThanOrEqual(44);
    expect(box.height).toBeGreaterThanOrEqual(44);
    await page.screenshot({ path: testInfo.outputPath(`device-keys-${width}.png`), fullPage: true });
  }
});

test('storage restrictions do not prevent reading and are explained in Device', async ({ page }) => {
  await page.addInitScript(() =>
    Object.defineProperty(window, 'localStorage', {
      get() {
        throw new DOMException('Storage unavailable', 'SecurityError');
      }
    })
  );
  await readTag(page);
  await page.locator('[data-view-target="device"]').click();
  await expect(page.locator('#remembered-keys-status')).toHaveText('Browser storage unavailable');
  await expect(page.locator('#remembered-keys-clear')).toBeDisabled();
});
