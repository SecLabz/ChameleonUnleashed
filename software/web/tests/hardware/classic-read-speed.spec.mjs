import { test, expect } from '@playwright/test';
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';

const deviceURL = 'http://192.168.16.1';
const hash = (value) => createHash('sha256').update(JSON.stringify(value)).digest('hex');

// Real, read-only Classic tag. No PM3 and no tag/slot/settings mutation.
test('measure the complete Classic UI read identically over real USB and BLE', async ({
  page,
  context,
  request
}, testInfo) => {
  test.skip(process.env.CHAMELEON_BLE_TEST_CLASSIC_SPEED !== '1', 'Requires a placed Classic tag');
  const rounds = Number(process.env.CHAMELEON_BLE_TEST_CLASSIC_SPEED_ROUNDS || 2);
  const readTimeout = Number(process.env.CHAMELEON_BLE_TEST_CLASSIC_SPEED_TIMEOUT || 90000);
  expect([1, 2, 3]).toContain(rounds);
  expect(readTimeout).toBeGreaterThanOrEqual(15000);
  expect(readTimeout).toBeLessThanOrEqual(900000);
  test.setTimeout(2 * rounds * readTimeout + 60000);
  page.setDefaultTimeout(15000);
  // Optional private recovered capture: seed a normal dictionary, never trusted key mappings.
  const dictionaryCapture = process.env.CHAMELEON_BLE_TEST_CLASSIC_SPEED_DICTIONARY_CAPTURE;
  if (dictionaryCapture) {
    const capture = JSON.parse(await readFile(dictionaryCapture, 'utf8')).document;
    const keys = capture.recoveredKeys.flatMap((sector) => [sector.key_a, sector.key_b]).filter(Boolean);
    expect(keys.every((key) => Array.isArray(key) && key.length === 6)).toBe(true);
    const dictionary = [
      ...new Set(keys.map((key) => key.map((b) => b.toString(16).padStart(2, '0')).join('')))
    ];
    await context.addInitScript(
      (text) => localStorage.setItem('chameleon-dictionary-v1', text),
      dictionary.join('\n')
    );
  }
  const legacyApp = process.env.CHAMELEON_BLE_TEST_CLASSIC_SPEED_LEGACY_APP;
  const frontendApp = process.env.CHAMELEON_BLE_TEST_CLASSIC_SPEED_FRONTEND_APP;
  if (frontendApp || legacyApp) {
    const body = await readFile(frontendApp || legacyApp);
    await context.route('**/app.mjs', (route) => route.fulfill({ body, contentType: 'text/javascript' }));
  }
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const slots = await usbSlotSnapshot(request, deviceURL);
  await context.route(RESOURCE_BASE + '**', async (route) => {
    const path = route.request().url().slice(RESOURCE_BASE.length);
    const bytes = await readFile(new URL('../../../../.cache/web/cdn/' + path, import.meta.url));
    await route.fulfill({ body: bytes, headers: { 'Access-Control-Allow-Origin': '*' } });
  });
  const usbPage = await context.newPage();
  const chooser = await physicalChooser(page);
  const saveTrace = await traceGatt(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  const usbCommands = [];
  const starts = new Map();
  usbPage.on('request', (r) => starts.set(r, performance.now()));
  usbPage.on('response', (r) => {
    const number = /\/api\/command\/(\d+)/.exec(r.url());
    if (number) {
      usbCommands.push({
        command: Number(number[1]),
        elapsedMs: performance.now() - starts.get(r.request()),
        bytes: r.request().postDataBuffer()?.length
      });
    }
  });
  const results = {
    firmware: before.git,
    providedDictionary: Boolean(dictionaryCapture),
    legacyFrontend: Boolean(legacyApp),
    frontendOverride: Boolean(frontendApp),
    rounds: [],
    tagWrites: false,
    slotWrites: false
  };
  let reference;
  let gatt;
  const read = async (target, transport) => {
    target.on('console', (message) => {
      const text = message.text();
      if (text.startsWith('CLASSIC_SPEED_PHASE:')) {
        console.log(`${transport}: ${text.slice(20)}`);
      }
    });
    await target.locator('[data-view-target="scan"]').click();
    await target.locator('#scan').click();
    await expect(target.locator('#studio-document')).toBeVisible();
    await expect(target.locator('#stop')).toBeHidden();
    for (let round = 0; round < rounds; round++) {
      await target.evaluate(() => {
        globalThis.classicReadClock = { phases: [] };
        const clock = globalThis.classicReadClock;
        new MutationObserver((_, observer) => {
          const status = document.querySelector('#status')?.textContent;
          if (status && clock.phases.at(-1)?.phase !== status) {
            clock.phases.push({ phase: status, at: performance.now() });
            console.debug('CLASSIC_SPEED_PHASE:' + status);
          }
          if (clock.started && status !== 'Read complete') {
            clock.sawWork = true;
          }
          if (clock.started && clock.sawWork && status === 'Read complete') {
            clock.ended = performance.now();
            observer.disconnect();
          }
        }).observe(document.body, {
          childList: true,
          subtree: true,
          characterData: true
        });
      });
      await target.locator('#read').click();
      const confirmation = target.getByRole('dialog', { name: 'Read fresh memory?', exact: true });
      if (round) {
        await expect(confirmation).toBeVisible();
        await target.evaluate(() => {
          const button = [...document.querySelectorAll('dialog button')].find(
            (b) => b.textContent === 'Recover'
          );
          button.addEventListener(
            'click',
            () => {
              globalThis.classicReadClock.started = performance.now();
            },
            { once: true, capture: true }
          );
        });
        await confirmation.getByRole('button', { name: 'Recover', exact: true }).click();
      }
      await target.waitForFunction(
        () => {
          const clock = globalThis.classicReadClock;
          return clock.started && clock.sawWork && document.querySelector('#stop').hidden;
        },
        null,
        { timeout: readTimeout }
      );
      await expect(target.locator('#status')).toHaveText('Read complete');
      await expect(target.locator('#stop')).toBeHidden();
      await target.waitForFunction(() => Number.isFinite(globalThis.classicReadClock.ended), null, {
        timeout: readTimeout
      });
      const clock = await target.evaluate(() => globalThis.classicReadClock);
      expect(clock.ended - clock.started).toBeGreaterThan(0);
      let button = target.locator('#export-json');
      if (!(await button.isVisible())) {
        await target.locator('#export-menu summary').click();
        button = target.locator('#export-document');
      }
      const pending = target.waitForEvent('download');
      await button.click();
      const bytes = await readFile(await (await pending).path());
      const capture = JSON.parse(bytes).document;
      await writeFile(testInfo.outputPath(`${transport}-${round}.json`), bytes);
      const observed = { card: capture.card, memory: capture.memory };
      expect(capture.kind).toBe('classic');
      expect(capture.memory.every((b) => Array.isArray(b) && b.length === 16)).toBe(true);
      if (!reference) {
        reference = observed;
      }
      expect(observed).toEqual(reference);
      results.rounds.push({
        transport,
        round,
        elapsedMs: clock.ended - clock.started,
        blocks: capture.memory.length,
        sha256: hash(observed),
        phases: clock.phases
      });
    }
  };
  // Capture the initial Read click before the application starts its work.
  await context.addInitScript(() =>
    document.addEventListener(
      'click',
      (event) => {
        if (event.target.closest('#read') && globalThis.classicReadClock) {
          globalThis.classicReadClock.started = performance.now();
        }
      },
      true
    )
  );
  try {
    await usbPage.goto(deviceURL + '/#device');
    await expect(usbPage.locator('#device-info')).toContainText(before.chip);
    await read(usbPage, 'usb');
    await usbPage.close();
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await read(page, 'ble');
    chooser();
    gatt = await saveTrace(testInfo.outputPath('gatt-trace.json'));
    expect(gatt.errors).toEqual([]);
    expect(gatt.truncated).toBe(false);
    expect(leakedHttp).toEqual([]);
    const mutations = [2030, 2033, 2034, 2038, 2039];
    expect(usbCommands.filter((x) => mutations.includes(x.command))).toEqual([]);
    expect(gatt.requests.filter((x) => x.operation === 8 && mutations.includes(x.argument))).toEqual([]);
  } finally {
    await usbPage.close();
    await page.close();
    await expect.poll(async () => (await info()).busy).toBe(false);
    const after = await info();
    for (const key of ['chip', 'git', 'reader', 'active_slot', 'settings']) {
      expect(after[key]).toEqual(before[key]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(slots);
    await writeFile(testInfo.outputPath('usb-commands.json'), JSON.stringify(usbCommands, null, 2));
    results.gattOperations = gatt?.requests.length;
    results.gattWrites = gatt?.chunks.filter((x) => x.direction === 'request').length;
    results.preserved = true;
    await writeFile(testInfo.outputPath('speed.json'), JSON.stringify(results, null, 2));
  }
});
