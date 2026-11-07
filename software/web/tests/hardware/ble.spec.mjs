import { test, expect } from '@playwright/test';
import { physicalChooser } from './chooser.mjs';
import { decodeSlot } from '../../src/device/slots.mjs';

test('the physical Ultra serves device information and slot images over Web Bluetooth', async ({
  page,
  request
}, testInfo) => {
  const before = await (await request.get('http://192.168.16.1/api/device')).json();
  expect(before.busy).toBe(false);
  const active = decodeSlot(
    new Uint8Array(await (await request.get(`http://192.168.16.1/api/slot/${before.active_slot}`)).body())
  );
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  let bluetoothPhase = false;
  await page.route('**/api/**', async (route) => {
    if (bluetoothPhase) {
      leakedHttp.push(route.request().url());
      await route.abort();
    } else {
      await route.continue();
    }
  });
  try {
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText('Bluetooth');
    await expect(page.locator('#device-info')).toContainText(before.chip);
    if (!before.reader && active.hf.enabled && [1500, 1501, 2000].includes(active.hf.type)) {
      await expect(page.locator('#connection-emulation')).toHaveText(
        `${active.hf.type === 2000 ? 'NFC-B' : 'NFC-V'} emulation of slot ${before.active_slot + 1} starts once Bluetooth disconnects.`
      );
    } else {
      await expect(page.locator('#connection-emulation')).toBeHidden();
    }
    bluetoothPhase = true;
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    await expect(page.locator('#slots .slot-card')).toHaveCount(8);
    await page.locator('#slot-0 .bay-open').first().click();
    await expect(page.locator('#studio-document')).toBeVisible();
    await expect(page.locator('#stop')).toBeHidden();
    await page.screenshot({ path: testInfo.outputPath('physical-ble-studio.png'), fullPage: true });
    // Connection controls live in the Device view; the open document must survive.
    await page.getByRole('button', { name: 'Device', exact: true }).click();
    await page.getByRole('button', { name: 'Disconnect Bluetooth', exact: true }).click();
    await expect(page.locator('#connection-help')).toHaveText(
      'Bluetooth disconnected. Reconnect to control the device.'
    );
    await expect(page.locator('#disconnect-bluetooth')).toBeHidden();
    await expect(page.locator('#connection-emulation')).toBeHidden();
    await expect(page.locator('#toast')).not.toContainText('connection lost');
    await page.getByRole('button', { name: 'Studio', exact: true }).click();
    await expect(page.locator('#studio-document')).toBeVisible();
    expect(leakedHttp).toEqual([]);
    checkChooser();
    await page.screenshot({ path: testInfo.outputPath('physical-disconnected-studio.png'), fullPage: true });
  } finally {
    bluetoothPhase = false;
    await page.getByRole('button', { name: 'Device', exact: true }).click();
    const usb = page.getByRole('button', { name: 'Use USB', exact: true });
    if (await usb.isVisible()) {
      await usb.click();
      await expect(page.locator('#connection-help')).toContainText('USB');
    } else {
      await page.close();
    }
  }
  await expect
    .poll(async () => (await (await request.get('http://192.168.16.1/api/device')).json()).busy)
    .toBe(false);
  const after = await (await request.get('http://192.168.16.1/api/device')).json();
  expect(after.chip).toBe(before.chip);
  expect(after.active_slot).toBe(before.active_slot);
  expect(after.reader).toBe(before.reader);
  expect(after.settings).toEqual(before.settings);
  expect(after.busy).toBe(false);
  await testInfo.attach('physical-ble-evidence', {
    body: JSON.stringify(
      {
        chip: after.chip,
        git: after.git,
        activeSlot: after.active_slot,
        reader: after.reader,
        httpFallbackRequests: leakedHttp,
        explicitDisconnect: true,
        documentRetainedAfterDisconnect: true,
        readOnly: true
      },
      null,
      2
    ),
    contentType: 'application/json'
  });
});
