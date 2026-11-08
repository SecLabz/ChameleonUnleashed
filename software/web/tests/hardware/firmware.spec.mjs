import { test, expect } from '@playwright/test';
import { physicalChooser } from './chooser.mjs';

const firmware = process.env.CHAMELEON_DFU_TEST_ZIP;
test('the physical Ultra installs a Nordic application ZIP over Bluetooth DFU', async ({
  page,
  request
}, testInfo) => {
  test.skip(!firmware, 'Set CHAMELEON_DFU_TEST_ZIP to select the firmware ZIP');
  test.setTimeout(15 * 60 * 1000);
  const before = await (await request.get('http://192.168.16.1/api/device', { timeout: 15000 })).json();
  expect(before.busy).toBe(false);
  const checkChooser = await physicalChooser(page);
  await page.addInitScript(() => {
    window.dfuWrites = {};
    const original = BluetoothRemoteGATTCharacteristic.prototype.writeValueWithoutResponse;
    BluetoothRemoteGATTCharacteristic.prototype.writeValueWithoutResponse = function (bytes) {
      if (this.uuid === '8ec90002-f315-4f60-9fb8-838830daea50') {
        window.dfuWrites[bytes.byteLength] = (window.dfuWrites[bytes.byteLength] ?? 0) + 1;
      }
      return original.call(this, bytes);
    };
  });
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.goto('./#device');
  await page.locator('#connect-bluetooth').click();
  await expect(page.locator('#device-info')).toContainText(before.chip);
  await page.locator('#firmware-update-link').click();
  const modal = page.getByRole('dialog', { name: 'Firmware update', exact: true });
  await modal.getByLabel('Nordic firmware ZIP').setInputFiles(firmware);
  await modal.getByRole('button', { name: 'Install selected ZIP', exact: true }).click();
  const confirm = page.getByRole('dialog', { name: 'Install firmware?', exact: true });
  await expect(confirm).toContainText('classic DFU');
  await confirm.getByRole('button', { name: 'Install', exact: true }).click();
  const select = modal.getByRole('button', { name: 'Select Ultra in DFU', exact: true });
  await expect(select).toBeEnabled();
  const started = Date.now();
  await select.click();
  let previous = -1;
  while (await modal.getByRole('button', { name: 'Stop', exact: true }).isVisible()) {
    if (Date.now() - started > 12 * 60 * 1000) {
      throw new Error(await modal.locator('#fw-message').textContent());
    }
    const progress = await modal.locator('progress').evaluate((node) => Math.floor(node.value * 10));
    if (progress !== previous) {
      console.log(`DFU: ${progress * 10}%`);
      previous = progress;
    }
    await page.waitForTimeout(1000);
  }
  await expect(modal.locator('#fw-message')).toContainText('Firmware transferred');
  const elapsedMs = Date.now() - started;
  await modal.getByRole('button', { name: 'Reconnect and verify', exact: true }).click();
  await expect(modal.locator('#fw-title')).toHaveText('Your Ultra is up to date', { timeout: 60000 });
  await page.screenshot({ path: testInfo.outputPath('physical-ble-update.png') });
  await modal.getByRole('button', { name: 'Return to Chameleon', exact: true }).click();
  await expect(page.locator('#device-info')).toContainText('Connected over Bluetooth');
  const rows = page.locator('#device-rows');
  if (process.env.CHAMELEON_DFU_EXPECT_GIT) {
    await expect(rows).toContainText(process.env.CHAMELEON_DFU_EXPECT_GIT);
  }
  await expect(rows).toContainText(before.chip);
  expect(errors).toEqual([]);
  checkChooser();
  await testInfo.attach('physical-dfu', {
    body: JSON.stringify({
      before: before.git,
      after: await rows.innerText(),
      elapsedMs,
      writes: await page.evaluate(() => window.dfuWrites)
    }),
    contentType: 'application/json'
  });
  await page.getByRole('button', { name: 'Disconnect Bluetooth', exact: true }).click();
});
