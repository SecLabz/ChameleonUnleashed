import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { RESOURCE_BASE } from '../../../../.cache/web/assets/modules/runtime/assets.mjs';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';
import { usbSlotSnapshot } from './usb-slot-snapshot.mjs';

const deviceURL = 'http://192.168.16.1';

test('Stop during real Classic hardnested acquisition releases BLE and retains partial observations', async ({
  page,
  context,
  request
}, testInfo) => {
  test.skip(process.env.CHAMELEON_BLE_TEST_CLASSIC_STOP !== '1', 'Requires a placed hardnested Classic tag');
  test.setTimeout(120000);
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
  const chooser = await physicalChooser(page);
  const saveTrace = await traceGatt(page);
  const leakedHttp = [];
  let connected = false;
  await page.route('**/api/**', (route) => {
    if (connected) {
      leakedHttp.push(route.request().url());
    }
    return route.abort();
  });
  let stoppedMs;
  let knownBlocks;
  try {
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    connected = true;
    await page.locator('[data-view-target="scan"]').click();
    await page.locator('#scan').click();
    await expect(page.locator('#studio-document')).toBeVisible();
    await expect(page.locator('#stop')).toBeHidden();
    await page.locator('#read').click();
    await expect(page.locator('#status')).toContainText('Hardnested acquisition', { timeout: 60000 });
    // Wait for actual autonomous RF results, not just the acquisition heading.
    await expect(page.locator('#status')).toContainText(/Hardnested acquisition · [1-9]\d*\//);
    const started = performance.now();
    await page.locator('#stop').click();
    await expect(page.locator('#stop')).toBeHidden();
    stoppedMs = performance.now() - started;
    await expect(page.locator('#status')).toHaveText('Stopped · partial results retained');
    await expect.poll(async () => (await info()).busy).toBe(false);
    const lines = (await page.locator('#memory textarea').inputValue()).split('\n');
    knownBlocks = lines.filter((line) => /^[\da-f\s]{32,}$/i.test(line)).length;
    expect(knownBlocks).toBeGreaterThan(0);
    expect(knownBlocks).toBeLessThan(64);
    const gatt = await saveTrace(testInfo.outputPath('gatt-trace.json'));
    expect(gatt.errors).toEqual([]);
    expect(gatt.truncated).toBe(false);
    expect(gatt.requests.some((x) => x.operation === 25)).toBe(true);
    expect(gatt.requests.some((x) => x.operation === 26)).toBe(true);
    expect(gatt.streamEvents.length).toBeGreaterThan(0);
    const writes = [2030, 2033, 2034, 2038, 2039];
    expect(gatt.requests.filter((x) => x.operation === 8 && writes.includes(x.argument))).toEqual([]);
    expect(leakedHttp).toEqual([]);
    chooser();
  } finally {
    await page.close();
    await expect.poll(async () => (await info()).busy).toBe(false);
    const after = await info();
    for (const field of ['chip', 'git', 'reader', 'active_slot', 'settings']) {
      expect(after[field]).toEqual(before[field]);
    }
    expect(await usbSlotSnapshot(request, deviceURL)).toEqual(slots);
    await testInfo.attach('physical-classic-stop', {
      body: JSON.stringify({
        firmware: before.git,
        stoppedMs,
        knownBlocks,
        preserved: true,
        pm3Used: false,
        tagWrites: false
      }),
      contentType: 'application/json'
    });
  }
});
