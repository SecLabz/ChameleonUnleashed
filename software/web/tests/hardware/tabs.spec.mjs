import { test, expect } from '@playwright/test';
import { physicalChooser } from './chooser.mjs';

const deviceURL = 'http://192.168.16.1';

test('real browser tabs reject a second BLE owner and reconnect after release or tab close', async ({
  page,
  context,
  request
}, testInfo) => {
  test.setTimeout(120000);
  const before = await (await request.get(deviceURL + '/api/device')).json();
  expect(before.busy).toBe(false);
  const second = await context.newPage();
  const firstChooser = await physicalChooser(page);
  const secondChooser = await physicalChooser(second);
  const leakedHttp = [];
  const bluetoothPhase = new Map([
    [page, false],
    [second, false]
  ]);
  for (const tab of [page, second]) {
    await tab.route('**/api/**', (route) => {
      if (bluetoothPhase.get(tab)) {
        leakedHttp.push({ tab: tab === page ? 'first' : 'second', url: route.request().url() });
      }
      return route.abort();
    });
  }
  const connect = async (tab, label) => {
    await tab.bringToFront();
    bluetoothPhase.set(tab, true);
    await tab.getByRole('button', { name: label, exact: true }).click();
    await expect(tab.locator('#device-info')).toContainText(before.chip);
    await expect(tab.locator('#connection-help')).toHaveText('Connected over Bluetooth');
  };
  const locks = (tab) =>
    tab.evaluate(async () =>
      (await navigator.locks.query()).held
        .filter((lock) => lock.name.startsWith('chameleon-bluetooth:'))
        .map(({ name, clientId }) => ({ name, clientId }))
    );
  const evidence = [];
  let qualified = false;
  try {
    await page.goto('./#device');
    await connect(page, 'Connect Bluetooth');
    const firstLocks = await locks(page);
    expect(firstLocks).toHaveLength(1);
    evidence.push({ stage: 'first owner', locks: firstLocks });
    await second.goto('./#device');
    await second.bringToFront();
    bluetoothPhase.set(second, true);
    await second.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(second.locator('#toast')).toContainText('already open in another tab');
    await expect(page.locator('#connection-help')).toHaveText('Connected over Bluetooth');
    await page.bringToFront();
    await page.locator('#refresh-device').click();
    await expect(page.locator('#refresh-device')).toBeEnabled();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    expect(await locks(second)).toEqual(firstLocks);

    await page.getByRole('button', { name: 'Disconnect Bluetooth', exact: true }).click();
    await expect.poll(() => locks(second)).toEqual([]);
    await connect(second, 'Connect Bluetooth');
    const secondLocks = await locks(second);
    expect(secondLocks).toHaveLength(1);
    expect(secondLocks[0].name).toBe(firstLocks[0].name);
    expect(secondLocks[0].clientId).not.toBe(firstLocks[0].clientId);
    evidence.push({ stage: 'second owner after explicit release', locks: secondLocks });

    await second.close();
    await expect.poll(() => locks(page)).toEqual([]);
    await connect(page, 'Reconnect Bluetooth');
    await page.getByRole('button', { name: 'Slots', exact: true }).click();
    await expect(page.locator('#slots .slot-card')).toHaveCount(8);
    evidence.push({ stage: 'first owner after second tab closed', locks: await locks(page) });
    firstChooser();
    secondChooser();
    expect(leakedHttp).toEqual([]);
    qualified = true;
  } finally {
    for (const tab of [page, second]) {
      if (!tab.isClosed()) {
        await tab.close();
      }
    }
    await expect
      .poll(async () => (await (await request.get(deviceURL + '/api/device')).json()).busy)
      .toBe(false);
    const after = await (await request.get(deviceURL + '/api/device')).json();
    for (const key of ['chip', 'reader', 'active_slot', 'settings']) {
      expect(after[key], key).toEqual(before[key]);
    }
    await testInfo.attach('physical-tab-ownership', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          evidence,
          httpFallbackRequests: leakedHttp
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
