import { createHash } from 'node:crypto';
import { test, expect } from '@playwright/test';
import { physicalChooser } from './chooser.mjs';

const deviceURL = 'http://192.168.16.1';
const fields = ['animation', 'press_a', 'press_b', 'long_a', 'long_b', 'sleep', 'long_ms'];

// Explicit opt-in: persists reversible settings and reboots the actual Ultra.
// Recovery only enters/reboots the existing application; no update is begun.
test('real BLE settings survive reboot and the restored originals survive a second reboot', async ({
  page,
  request
}, testInfo) => {
  test.skip(
    process.env.CHAMELEON_BLE_TEST_SETTINGS_REBOOT !== '1',
    'Requires reversible settings and USB reboot'
  );
  test.setTimeout(300000);
  const get = async (path) => {
    const response = await request.get(deviceURL + path, { timeout: 3000 });
    expect(response.status(), path).toBe(200);
    return response;
  };
  const post = async (path, data = Buffer.alloc(0)) => {
    const response = await request.post(deviceURL + path, { data, timeout: 10000 });
    expect(response.status(), path).toBe(200);
    return response;
  };
  const info = async () => (await get('/api/device')).json();
  const status = async () => (await get('/api/update')).json();
  const waitMode = async (mode) => {
    await expect
      .poll(
        async () => {
          try {
            return (await status()).mode;
          } catch {
            return 'USB reconnecting';
          }
        },
        { timeout: 60000, intervals: [250, 500, 1000] }
      )
      .toBe(mode);
    return status();
  };
  const initialUpdate = await status();
  expect(initialUpdate.mode).toBe('application');
  expect(initialUpdate.layout).toBe(2);
  const before = await info();
  expect(before.busy).toBe(false);
  const snapshot = async () => {
    const metadata = [];
    const images = [];
    const session = (await (await post('/api/slots/session')).json()).session;
    try {
      for (let slot = 0; slot < 8; slot++) {
        const meta = await (await get(`/api/slot/${slot}`)).body();
        metadata.push(meta.toString('hex'));
        for (const [sense, offset] of [
          [2, 0],
          [1, 2]
        ]) {
          if (!meta.readUInt16BE(offset)) {
            continue;
          }
          const opened = await (
            await post(`/api/slots/open?id=${session}`, Buffer.from([slot, sense]))
          ).body();
          expect(opened.length).toBe(6);
          const size = opened.readUInt16BE(0);
          const parts = [];
          for (let start = 0; start < size; start += 512) {
            const count = Math.min(512, size - start);
            const payload = Buffer.alloc(4);
            payload.writeUInt16BE(start, 0);
            payload.writeUInt16BE(count, 2);
            const part = await (await post(`/api/slots/read?id=${session}`, payload)).body();
            expect(part.length).toBe(count);
            parts.push(part);
          }
          images.push({
            slot,
            sense,
            size,
            sha256: createHash('sha256').update(Buffer.concat(parts)).digest('hex')
          });
        }
      }
    } finally {
      await post(`/api/end?id=${session}`);
    }
    return { metadata, images };
  };
  const originalSlots = await snapshot();
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  const changedSettings = { ...before.settings };
  for (const key of fields) {
    const value = Number(before.settings[key]);
    if (key === 'long_ms') {
      changedSettings[key] = value === 65535 ? 65534 : value + 1;
    } else if (key === 'sleep') {
      changedSettings[key] = value === 60 ? 59 : value + 1;
    } else {
      changedSettings[key] = (value + 1) % (key === 'animation' ? 4 : 6);
    }
  }
  const connect = async (label) => {
    await page.bringToFront();
    await page.getByRole('button', { name: label, exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await expect(page.locator('#connection-help')).toHaveText('Connected over Bluetooth');
  };
  const change = async (key, value) => {
    const field = page.locator(`[data-setting="${key}"]`);
    await expect(field).toBeEnabled();
    if (['sleep', 'long_ms'].includes(key)) {
      await field.fill(String(value));
      await field.dispatchEvent('change');
    } else {
      await field.selectOption(String(value));
    }
    await expect.poll(async () => (await info()).settings[key]).toBe(value);
    await expect(field).toBeEnabled();
  };
  const rebootEvidence = [];
  const returnFromRecovery = async () => {
    const recovery = await status();
    expect(recovery.mode).toBe('recovery');
    expect(recovery.valid).toBe(true);
    expect(recovery.session).toBe(0);
    expect(recovery.size).toBe(0);
    expect(recovery.received).toBe(0);
    expect(recovery.recovery_crc32).toBe(initialUpdate.recovery_crc32);
    await post('/api/update/reboot');
    expect(await waitMode('application')).toEqual(initialUpdate);
    rebootEvidence.push({ recovery, application: await status() });
  };
  const reboot = async () => {
    expect((await info()).busy).toBe(false);
    await post('/api/update/enter');
    await waitMode('recovery');
    await returnFromRecovery();
  };
  const assertDevice = async (settings) => {
    const current = await info();
    for (const key of ['chip', 'git', 'reader', 'active_slot']) {
      expect(current[key], key).toEqual(before[key]);
    }
    expect(current.settings).toEqual(settings);
    expect(current.busy).toBe(false);
    expect(await snapshot()).toEqual(originalSlots);
  };
  let changed = false;
  let restoredAndRebooted = false;
  let qualified = false;
  const fallbackRestores = [];
  try {
    await page.goto('./#device');
    await connect('Connect Bluetooth');
    changed = true;
    for (const key of fields) {
      await change(key, changedSettings[key]);
    }
    expect((await info()).settings).toEqual(changedSettings);
    await page.getByRole('button', { name: 'Disconnect Bluetooth', exact: true }).click();
    await reboot();
    await assertDevice(changedSettings);
    await connect('Reconnect Bluetooth');
    for (const key of fields) {
      await expect(page.locator(`[data-setting="${key}"]`)).toHaveValue(String(changedSettings[key]));
    }
    for (const key of fields) {
      await change(key, before.settings[key]);
    }
    await page.getByRole('button', { name: 'Disconnect Bluetooth', exact: true }).click();
    await reboot();
    await assertDevice(before.settings);
    restoredAndRebooted = true;
    await connect('Reconnect Bluetooth');
    for (const key of fields) {
      await expect(page.locator(`[data-setting="${key}"]`)).toHaveValue(String(before.settings[key]));
    }
    checkChooser();
    expect(leakedHttp).toEqual([]);
    qualified = true;
  } finally {
    await page.close();
    await expect
      .poll(
        async () => {
          try {
            return ['application', 'recovery'].includes((await status()).mode);
          } catch {
            return false;
          }
        },
        { timeout: 60000 }
      )
      .toBe(true);
    if ((await status()).mode === 'recovery') {
      await returnFromRecovery();
    }
    await expect.poll(async () => (await info()).busy).toBe(false);
    if (changed && !restoredAndRebooted) {
      const current = await info();
      for (const [id, key] of fields.entries()) {
        if (current.settings[key] === before.settings[key]) {
          continue;
        }
        const value = before.settings[key];
        const payload = id === 6 ? [id, value >> 8, value & 255] : [id, value];
        expect([...(await (await post('/api/settings', Buffer.from(payload))).body())]).toEqual([0, 0x68]);
        fallbackRestores.push(key);
      }
      await reboot();
    }
    await assertDevice(before.settings);
    await testInfo.attach('physical-settings-reboot-evidence', {
      body: JSON.stringify(
        {
          qualified,
          chip: before.chip,
          firmware: before.git,
          changedSettings,
          restoredSettings: before.settings,
          rebootEvidence,
          slots: originalSlots,
          fallbackRestores,
          httpFallbackRequests: leakedHttp,
          firmwareUpdateStarted: false,
          originalSettingsPersisted: true,
          pairingChanged: false,
          pinChanged: false
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
