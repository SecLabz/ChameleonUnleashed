import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { physicalChooser } from './chooser.mjs';
import { traceGatt } from './gatt-trace.mjs';

const deviceURL = 'http://192.168.16.1';
const fields = ['animation', 'press_a', 'press_b', 'long_a', 'long_b', 'sleep', 'long_ms'];

// Persistent changes are opt-in. Pairing, PIN and physical RF tags are untouched.
test('physical BLE settings save, reject an occupied device and restore every original value', async ({
  page,
  request
}, testInfo) => {
  test.skip(process.env.CHAMELEON_BLE_TEST_SETTINGS !== '1', 'Requires reversible settings qualification');
  test.setTimeout(180000);
  const frontend = process.env.CHAMELEON_BLE_TEST_SETTINGS_APP;
  if (frontend) {
    const body = await readFile(frontend);
    await page.route('**/app.mjs', (route) => route.fulfill({ body, contentType: 'text/javascript' }));
  }
  const saveTrace = await traceGatt(page);
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  const metadata = [];
  for (let slot = 0; slot < 8; slot++) {
    metadata.push(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body());
  }
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  let bluetooth = false;
  let usbSession = 0;
  let qualified = false;
  const evidence = [];
  const fallbackRestores = [];
  await page.route('**/api/**', (route) => {
    if (bluetooth) {
      leakedHttp.push(route.request().url());
      return route.abort();
    }
    return route.continue();
  });

  function alternate(key) {
    const original = Number(before.settings[key]);
    if (key === 'long_ms') {
      return original === 65535 ? 65534 : original + 1;
    }
    if (key === 'sleep') {
      return original === 60 ? 59 : original + 1;
    }
    return (original + 1) % (key === 'animation' ? 4 : 6);
  }

  async function change(key, value) {
    const field = page.locator(`[data-setting="${key}"]`);
    await expect(field).toBeEnabled();
    if (['sleep', 'long_ms'].includes(key)) {
      await field.fill(String(value));
      await field.dispatchEvent('change');
    } else {
      await field.selectOption(String(value));
    }
    await expect.poll(async () => (await info()).settings[key]).toBe(value);
    await expect(page.locator(`[data-setting="${key}"]`)).toHaveValue(String(value));
    await expect(page.locator(`[data-setting="${key}"]`)).toBeEnabled();
  }

  try {
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await expect(page.locator('#connection-help')).toHaveText('Connected over Bluetooth');
    bluetooth = true;
    for (const key of fields) {
      const start = Date.now();
      await change(key, alternate(key));
      const changed = await info();
      const expected = { ...before.settings, [key]: alternate(key) };
      expect(changed.settings, key).toEqual(expected);
      await change(key, before.settings[key]);
      expect((await info()).settings, `${key} restored`).toEqual(before.settings);
      evidence.push({ key, elapsedMs: Date.now() - start });
    }

    const reservation = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
    expect(reservation.status()).toBe(200);
    usbSession = (await reservation.json()).session;
    await page.locator('[data-setting="animation"]').selectOption(String(alternate('animation')));
    await expect(page.locator('#toast')).toContainText('Device busy');
    await expect(page.locator('[data-setting="animation"]')).toHaveValue(String(before.settings.animation));
    expect((await info()).settings).toEqual(before.settings);
    expect(
      (await request.post(`${deviceURL}/api/end?id=${usbSession}`, { data: Buffer.alloc(0) })).status()
    ).toBe(200);
    usbSession = 0;
    expect(leakedHttp).toEqual([]);
    checkChooser();
    const trace = await saveTrace(testInfo.outputPath('gatt-trace.json'));
    expect(trace.errors).toEqual([]);
    expect(trace.pending).toEqual([]);
    expect(trace.requests.filter((entry) => entry.operation === 9)).toHaveLength(15);
    qualified = true;
  } finally {
    bluetooth = false;
    if (!qualified && !page.isClosed()) {
      await saveTrace(testInfo.outputPath('gatt-trace.json'));
    }
    await page.close();
    if (usbSession) {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${usbSession}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    await expect.poll(async () => (await info()).busy).toBe(false);
    const current = await info();
    // Restore only values that still differ after a failed browser operation.
    for (const [key, id] of fields.map((key, id) => [key, id])) {
      if (current.settings[key] === before.settings[key]) {
        continue;
      }
      const value = before.settings[key];
      const payload = id === 6 ? [id, value >> 8, value & 255] : [id, value];
      const response = await request.post(deviceURL + '/api/settings', { data: Buffer.from(payload) });
      expect(response.status()).toBe(200);
      expect([...(await response.body())]).toEqual([0, 0x68]);
      fallbackRestores.push(key);
    }
    const after = await info();
    for (const key of ['chip', 'reader', 'active_slot', 'settings']) {
      expect(after[key], key).toEqual(before[key]);
    }
    for (let slot = 0; slot < 8; slot++) {
      expect(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body()).toEqual(metadata[slot]);
    }
    await testInfo.attach('physical-settings-evidence', {
      body: JSON.stringify(
        {
          qualified,
          chip: after.chip,
          firmware: after.git,
          fields: evidence,
          fallbackRestores,
          httpFallbackRequests: leakedHttp,
          pairingChanged: false,
          pinChanged: false,
          originalSettingsRestored: true
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
