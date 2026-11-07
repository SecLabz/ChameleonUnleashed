// One tab owns a device's GATT protocol until it disconnects.
export async function claimBluetoothDevice(device, locks = globalThis.navigator?.locks) {
  if (!locks) {
    return () => {};
  }
  let release;
  const held = new Promise((resolve) => {
    release = resolve;
  });
  return new Promise((resolve, reject) => {
    locks
      .request(`chameleon-bluetooth:${device.id}`, { ifAvailable: true }, async (lock) => {
        if (!lock) {
          reject(
            new Error('This Ultra is already open in another tab. Close its Bluetooth connection first.')
          );
          return;
        }
        resolve(release);
        await held;
      })
      .catch(reject);
  });
}
