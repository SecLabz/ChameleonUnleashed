import { expect } from '@playwright/test';

// DeviceAccess chooses a real advertised Ultra. No BluetoothEmulation domain
// is enabled: all GATT exchanges use the computer's physical adapter.
export async function physicalChooser(page) {
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('DeviceAccess.enable');
  const selected = new Set();
  const errors = [];
  cdp.on('DeviceAccess.deviceRequestPrompted', (event) => {
    if (selected.has(event.id)) {
      return;
    }
    const devices = event.devices.filter(
      (device) => !device.name || device.name.includes('ChameleonUltra') || device.name.startsWith('CU-')
    );
    if (devices.length === 1 && event.devices.length === 1) {
      selected.add(event.id);
      void cdp
        .send('DeviceAccess.selectPrompt', { id: event.id, deviceId: devices[0].id })
        .catch((error) => errors.push(error.message));
    }
  });
  return () => expect(errors).toEqual([]);
}
