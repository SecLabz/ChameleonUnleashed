import { test, expect } from '@playwright/test';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../../..', import.meta.url));
const config = JSON.parse(
  await readFile(new URL('../../../../.cache/web/assets/config.json', import.meta.url))
);
const generated = JSON.parse(
  execFileSync(
    'python3',
    [
      '-c',
      `
import sys,json,base64
sys.path.insert(0,'resource/tools')
from test_web_update import fixtures,archive
data=fixtures()
print(json.dumps({name:base64.b64encode(archive(*data[name],*(data['system'] if name=='legacy' else (None,None)))).decode() for name in ['normal','legacy']}))
`
    ],
    { cwd: root, encoding: 'utf8' }
  )
);
const files = Object.fromEntries(
  Object.entries(generated).map(([name, bytes]) => [name, Buffer.from(bytes, 'base64')])
);
const version = 0x1000001;

async function mockDevice(
  page,
  mode = 'application',
  { valid = true, rebootDelay = 0, rejectReboot = false } = {}
) {
  let runningVersion = version - 1;
  let received = 0;
  let systemReceived = 0;
  let committed = false;
  let restore = false;
  let flags;
  let rebootAt;
  let reboots = 0;
  // Obtain the real prefix CRC from the fixture without relying on ZIP internals.
  let recoveryCrc = Number(
    execFileSync(
      'python3',
      [
        '-c',
        `
import sys,struct
sys.path.insert(0,'resource/tools')
from test_web_update import fixtures
print(struct.unpack_from('<I',fixtures()['normal'][0],0x13ffc)[0])
`
      ],
      { cwd: root, encoding: 'utf8' }
    ).trim()
  );
  await page.route('**/api/update**', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const body = request.postDataBuffer() ?? Buffer.alloc(0);
    if (url.pathname === '/api/update') {
      if (rebootAt !== undefined && Date.now() >= rebootAt) {
        mode = 'application';
      }
      await route.fulfill({
        json: {
          mode,
          layout: 2,
          recovery_crc32: recoveryCrc,
          version: runningVersion,
          ...(mode === 'recovery' ? { valid } : {}),
          session: 42,
          received,
          size: 91021
        }
      });
      return;
    }
    if (url.pathname === '/api/update/enter') {
      mode = 'recovery';
    } else if (url.pathname === '/api/update/begin') {
      flags = body[0];
      restore = Boolean(flags & 1);
    } else if (url.pathname === '/api/update/system') {
      expect(Number(url.searchParams.get('offset'))).toBe(systemReceived);
      systemReceived += body.length;
    } else if (url.pathname === '/api/update/write') {
      if (restore) {
        expect(systemReceived).toBeGreaterThan(0);
      }
      expect(Number(url.searchParams.get('offset'))).toBe(received);
      received += body.length;
    } else if (url.pathname === '/api/update/commit') {
      committed = true;
    } else if (url.pathname === '/api/update/reboot') {
      reboots++;
      if (rejectReboot) {
        await route.fulfill({ status: 409, body: '' });
        return;
      }
      if (received || systemReceived) {
        expect(committed).toBe(true);
      }
      if (rebootDelay) {
        rebootAt = Date.now() + rebootDelay;
      } else {
        mode = restore ? 'restored' : 'application';
      }
      if (committed) {
        runningVersion = version;
      }
    }
    await route.fulfill({ status: 200, body: '' });
  });
  return {
    flags: () => flags,
    committed: () => committed,
    mode: () => mode,
    reboots: () => reboots,
    recoveryCrc: () => recoveryCrc,
    setRecoveryCrc: (value) => {
      recoveryCrc = value;
    }
  };
}

async function catalogue(page, count = 1, ready = Promise.resolve()) {
  const catalogue = {
    format: 1,
    sources: [
      { id: 'unleashed', label: 'Unleashed', releases: [{ version: 'v1.0.1', layout: 2, packages: {} }] },
      { id: 'official', label: 'Official', releases: [{ version: 'v2.3.0', layout: 0, packages: {} }] }
    ]
  };
  for (const [index, name, kind] of [
    [0, 'normal', 'application'],
    [1, 'legacy', 'full']
  ]) {
    const bytes = files[name];
    const identity = createHash('sha256').update(bytes).digest('hex');
    catalogue.sources[index].releases[0].packages[kind] = {
      size: bytes.length,
      sha256: identity,
      path: identity + '.zip'
    };
  }
  if (count > 1) {
    for (const source of catalogue.sources) {
      const release = source.releases[0];
      source.releases = Array.from({ length: count }, (_, index) => ({
        ...release,
        version: source.id === 'unleashed' ? `v1.0.${count - index}` : `v2.${count - index}.0`
      }));
    }
  }
  await page.route(config.baseUrl + '**', async (route) => {
    if (route.request().url().endsWith('/updates/catalog.json')) {
      await ready;
      await route.fulfill({ json: catalogue, headers: { 'Access-Control-Allow-Origin': '*' } });
    } else {
      const selected = Object.values(files).find((bytes) =>
        route
          .request()
          .url()
          .endsWith(createHash('sha256').update(bytes).digest('hex') + '.zip')
      );
      if (selected) {
        await route.fulfill({ body: selected, headers: { 'Access-Control-Allow-Origin': '*' } });
      } else {
        await route.abort();
      }
    }
  });
}

test('development channel installs only its explicitly selected snapshot', async ({ page }) => {
  const model = await mockDevice(page);
  await catalogue(page);
  const bytes = files.normal;
  const identity = createHash('sha256').update(bytes).digest('hex');
  const requests = [];
  await page.route(config.baseUrl + 'updates/dev/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    requests.push(path);
    if (path.endsWith('/catalog.json')) {
      await route.fulfill({
        json: {
          format: 1,
          sources: [
            {
              id: 'unleashed',
              label: 'Unleashed',
              releases: [
                {
                  version: 'v1.0.1-dev.abcdef0',
                  prerelease: true,
                  layout: 2,
                  packages: {
                    application: { size: bytes.length, sha256: identity, path: `files/${identity}.zip` }
                  }
                }
              ]
            }
          ]
        },
        headers: { 'Access-Control-Allow-Origin': '*' }
      });
    } else {
      await route.fulfill({ body: bytes, headers: { 'Access-Control-Allow-Origin': '*' } });
    }
  });
  await page.goto('/update.html');
  await expect(page.locator('#channel')).toHaveValue('stable');
  await expect(page.locator('#version')).toHaveValue('v1.0.1');
  expect(requests).toHaveLength(0);
  await page.locator('#channel').selectOption('development');
  await expect(page.locator('#version')).toHaveValue('v1.0.1-dev.abcdef0');
  await page.screenshot({ path: test.info().outputPath('development-channel.png'), fullPage: true });
  await page.locator('#update').click();
  await expect(page.locator('#title')).toHaveText('Your Ultra is up to date');
  expect(model.committed()).toBe(true);
  expect(requests.some((path) => path.endsWith(`/updates/dev/files/${identity}.zip`))).toBe(true);
});

test('development channel ignores a late catalogue after returning to stable', async ({ page }) => {
  await mockDevice(page);
  await catalogue(page);
  let release;
  const ready = new Promise((resolve) => {
    release = resolve;
  });
  await page.route(config.baseUrl + 'updates/dev/catalog.json', async (route) => {
    await ready;
    await route.fulfill({
      json: { format: 1, sources: [] },
      headers: { 'Access-Control-Allow-Origin': '*' }
    });
  });
  await page.goto('/update.html');
  await expect(page.locator('#version')).toHaveValue('v1.0.1');
  const requested = page.waitForRequest((request) => request.url().endsWith('/updates/dev/catalog.json'));
  await page.locator('#channel').selectOption('development');
  await requested;
  await expect(page.locator('#update')).toBeDisabled();
  await page.locator('#channel').selectOption('stable');
  await expect(page.locator('#version')).toHaveValue('v1.0.1');
  const response = page.waitForResponse((response) => response.url().endsWith('/updates/dev/catalog.json'));
  release();
  await response;
  await page.evaluate(
    () => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)))
  );
  await expect(page.locator('#version')).toHaveValue('v1.0.1');
  await expect(page.locator('#update')).toBeEnabled();
});

test('a late catalogue cannot unlock installation controls during a transfer', async ({ page }) => {
  await mockDevice(page);
  let releaseCatalogue;
  const catalogueReady = new Promise((resolve) => {
    releaseCatalogue = resolve;
  });
  await catalogue(page, 1, catalogueReady);
  let releaseWrite;
  const writeReady = new Promise((resolve) => {
    releaseWrite = resolve;
  });
  let writing;
  const firstWrite = new Promise((resolve) => {
    writing = resolve;
  });
  await page.route('**/api/update/write?**', async (route) => {
    writing();
    await writeReady;
    await route.fallback();
  });
  try {
    await page.goto('/update.html');
    await page
      .locator('#file')
      .setInputFiles({ name: 'firmware.zip', mimeType: 'application/zip', buffer: files.normal });
    await page.getByRole('button', { name: 'Install selected ZIP' }).click();
    await firstWrite;
    const lateReply = page.waitForResponse(config.baseUrl + 'updates/catalog.json');
    releaseCatalogue();
    await lateReply;
    await expect(page.locator('#source option')).toHaveCount(1);
    for (const id of ['update', 'source', 'version', 'file', 'wipe']) {
      await expect(page.locator('#' + id)).toBeDisabled();
    }
    releaseWrite();
    await expect(page.locator('#title')).toHaveText('Your Ultra is up to date');
  } finally {
    releaseCatalogue();
    releaseWrite();
  }
});

for (const changed of [false, true]) {
  test(`recovery confirmation uses the live device when recovery ${changed ? 'changed' : 'now matches'} since page load`, async ({
    page
  }) => {
    const device = await mockDevice(page);
    await catalogue(page);
    const recoveryCrc = device.recoveryCrc();
    if (!changed) {
      device.setRecoveryCrc(0);
    }
    const dialogs = [];
    page.on('dialog', async (dialog) => {
      dialogs.push(dialog.message());
      await dialog.dismiss();
    });
    await page.goto('/update.html');
    await expect(page.locator('#device')).toContainText('Ultra connected');
    device.setRecoveryCrc(changed ? 0 : recoveryCrc);
    await page.getByRole('button', { name: 'Install update', exact: true }).click();
    if (changed) {
      await expect.poll(() => dialogs.length).toBe(1);
      expect(dialogs[0]).toContain('replaces web recovery');
      expect(device.flags()).toBeUndefined();
    } else {
      await expect(page.locator('#title')).toHaveText('Your Ultra is up to date');
      expect(dialogs).toEqual([]);
    }
  });
}

test('an old file validation cannot enable a newer invalid selection', async ({ page }) => {
  await mockDevice(page);
  await catalogue(page);
  await page.goto('/update.html');
  await page.evaluate(() => {
    const original = File.prototype.arrayBuffer;
    let release;
    const ready = new Promise((resolve) => {
      release = resolve;
    });
    globalThis.releaseFileRead = release;
    File.prototype.arrayBuffer = async function () {
      if (this.name === 'first.zip') {
        await ready;
      }
      const bytes = await original.call(this);
      if (this.name === 'first.zip') {
        globalThis.firstFileReadFinished = true;
      }
      return bytes;
    };
  });
  await page
    .locator('#file')
    .setInputFiles({ name: 'first.zip', mimeType: 'application/zip', buffer: files.normal });
  await page
    .locator('#file')
    .setInputFiles({ name: 'invalid.zip', mimeType: 'application/zip', buffer: Buffer.from('invalid') });
  await expect(page.locator('#message')).toContainText('standard Nordic DFU ZIP');
  await page.evaluate(() => globalThis.releaseFileRead());
  await page.waitForFunction(() => globalThis.firstFileReadFinished);
  await expect(page.locator('#release')).toHaveText('invalid.zip');
  await expect(page.locator('#update')).toBeDisabled();
});

test('an oversized file is rejected before reading its contents', async ({ page }) => {
  await mockDevice(page);
  await catalogue(page);
  await page.goto('/update.html');
  await page.evaluate(() => {
    File.prototype.arrayBuffer = () => {
      throw new Error('File contents were read');
    };
  });
  await page.locator('#file').setInputFiles({
    name: 'too-large.zip',
    mimeType: 'application/zip',
    buffer: Buffer.alloc(4 * 1024 * 1024 + 1)
  });
  await expect(page.locator('#message')).toContainText('smaller than 4 MiB');
  await expect(page.locator('#update')).toBeDisabled();
});

test('the compact updater shows the selection once and keeps return navigation available', async ({
  page
}) => {
  await mockDevice(page);
  await catalogue(page, 5);
  await page.goto('/update.html');
  await page.locator('#source').selectOption('official');
  await page.locator('#version').selectOption('v2.3.0');
  await expect(page.locator('#source')).toHaveValue('official');
  await expect(page.locator('#version')).toHaveValue('v2.3.0');
  await expect(page.locator('#release')).toBeHidden();
  await expect(page.locator('#refresh')).toHaveCount(0);
  await expect(page.getByRole('link', { name: 'ZIP reader license' })).toHaveCount(0);
  await expect(page.locator('#restart')).toBeVisible();
  await expect(page.locator('#exit-hint')).toBeHidden();
});

test('an unreachable device is shown as disconnected and cannot start a transfer', async ({ page }) => {
  await page.route('**/api/update', (route) => route.abort());
  await catalogue(page);
  await page.goto('/update.html');
  await expect(page.locator('#device')).toHaveText('Ultra not connected');
  await expect(page.locator('#update')).toBeDisabled();
  await expect(page.locator('#restart')).toBeHidden();
});

test('phone browser offers five versions per source and installs a selected older release', async ({
  page
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockDevice(page);
  await catalogue(page, 5);
  await page.goto('/update.html');
  await expect(page.locator('#version option')).toHaveCount(5);
  await expect(page.locator('#version')).toHaveValue('v1.0.5');
  await page.locator('#source').selectOption('official');
  await expect(page.locator('#version option')).toHaveCount(5);
  await expect(page.locator('#version')).toHaveValue('v2.5.0');
  await expect(page.locator('#transition')).toBeVisible();
  await expect(page.locator('#wipe')).not.toBeChecked();
  await page.locator('#source').selectOption('unleashed');
  await page.locator('#version').selectOption('v1.0.1');
  await page.getByRole('button', { name: 'Install update', exact: true }).click();
  await expect(page.locator('#title')).toHaveText('Your Ultra is up to date');
  await expect(page.locator('#device')).toHaveText('Ultra connected · v1.0.1');
});

test('phone browser selects a release and installs its standard Nordic ZIP', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await mockDevice(page);
  await catalogue(page);
  await page.goto('/update.html');
  await expect(page.locator('#version')).toHaveValue('v1.0.1');
  await page.getByRole('button', { name: 'Install update', exact: true }).click();
  await expect(page.locator('#title')).toHaveText('Your Ultra is up to date');
  await expect(page.locator('#device')).toHaveText('Ultra connected · v1.0.1');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  expect(errors).toEqual([]);
  await page.locator('#source').selectOption('official');
  await expect(page.locator('#title')).toBeHidden();
  await expect(page.locator('#progress')).toBeHidden();
});

test('offline recovery installs a local Nordic ZIP at a 320px viewport', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 740 });
  await mockDevice(page, 'recovery');
  await page.route(config.baseUrl + '**', (route) => route.abort());
  await page.goto('/update.html');
  await expect(page.locator('#device')).toContainText('Recovery mode');
  await page.locator('details').click();
  await page
    .locator('#file')
    .setInputFiles({ name: 'ultra-dfu-app.zip', mimeType: 'application/zip', buffer: files.normal });
  await page.getByRole('button', { name: 'Install selected ZIP' }).click();
  await expect(page.locator('#title')).toHaveText('Your Ultra is up to date');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test('offline recovery exit waits for firmware before navigating home', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 740 });
  const device = await mockDevice(page, 'recovery', { rebootDelay: 800 });
  await page.route(config.baseUrl + '**', (route) => route.abort());
  let prematureNavigation = false;
  page.on('request', (request) => {
    if (new URL(request.url()).pathname === '/' && device.mode() !== 'application') {
      prematureNavigation = true;
    }
  });
  await page.goto('/update.html');
  const exit = page.getByRole('button', { name: 'Return to Chameleon' });
  await expect(exit).toBeInViewport();
  const home = new URL('/', page.url()).href;
  await exit.click();
  await expect(page).toHaveURL(home);
  expect(device.reboots()).toBe(1);
  expect(prematureNavigation).toBe(false);
});

test('invalid firmware cannot be selected as a recovery exit', async ({ page }) => {
  await mockDevice(page, 'recovery', { valid: false });
  await page.route(config.baseUrl + '**', (route) => route.abort());
  await page.goto('/update.html');
  await expect(page.locator('#device')).toContainText('Recovery mode');
  await expect(page.getByRole('button', { name: 'Return to Chameleon' })).toBeHidden();
});

test('a refused recovery exit keeps the page open and reports the error', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  const device = await mockDevice(page, 'recovery', { rejectReboot: true });
  await page.route(config.baseUrl + '**', (route) => route.abort());
  await page.goto('/update.html');
  const exit = page.getByRole('button', { name: 'Return to Chameleon' });
  await exit.click();
  await expect(page.locator('#message')).toContainText('busy');
  await expect(exit).toBeEnabled();
  await expect(page).toHaveURL(/\/update.html$/);
  expect(device.reboots()).toBe(1);
  expect(errors).toEqual([]);
});

test('cancelling before a recovery transfer restores the exit button', async ({ page }) => {
  const device = await mockDevice(page, 'recovery');
  await catalogue(page);
  await page.goto('/update.html');
  await page.getByLabel('Firmware source').selectOption('official');
  page.once('dialog', (dialog) => dialog.dismiss());
  await page.getByRole('button', { name: 'Install complete firmware' }).click();
  await expect(page.getByRole('button', { name: 'Return to Chameleon' })).toBeVisible();
  expect(device.flags()).toBeUndefined();
});

test('official release restores only after confirmation and makes erasure explicit', async ({ page }) => {
  const device = await mockDevice(page);
  await catalogue(page);
  await page.goto('/update.html');
  await page.getByLabel('Firmware source').selectOption('official');
  await expect(page.locator('#transition')).toBeVisible();
  await page.getByLabel('Erase saved tags and settings').check();
  const dismissed = new Promise((resolve) =>
    page.once('dialog', async (dialog) => {
      await dialog.dismiss();
      resolve();
    })
  );
  await page.getByRole('button', { name: 'Install complete firmware' }).click();
  await dismissed;
  await expect(page.getByRole('button', { name: 'Install complete firmware' })).toBeEnabled();
  expect(device.flags()).toBeUndefined();
  page.once('dialog', (dialog) => dialog.accept());
  await page.getByRole('button', { name: 'Install complete firmware' }).click();
  await expect(page.locator('#title')).toHaveText('Firmware transferred and verified');
  expect(device.flags()).toBe(3);
  expect(device.committed()).toBe(true);
  await expect(page.locator('#message')).toContainText('web interface is removed');
  for (const id of ['update', 'source', 'version', 'file', 'wipe']) {
    await expect(page.locator('#' + id)).toBeDisabled();
  }
  await expect(page.locator('#restart')).toBeHidden();
});
