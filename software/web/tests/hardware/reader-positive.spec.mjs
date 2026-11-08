import { test, expect } from '@playwright/test';
import { mkdir, readFile } from 'node:fs/promises';
import { physicalChooser } from './chooser.mjs';
import { startSimulation } from './pm3-simulation.mjs';

const deviceURL = 'http://192.168.16.1';
const uid = [0x11, 0x22, 0x33, 0x44];

test('the real BLE UI captures the same NFC-A identity as USB from a PM3 simulation', async ({
  page,
  request
}, testInfo) => {
  const executable = process.env.CHAMELEON_PM3_CLIENT;
  const port = process.env.CHAMELEON_PM3_PORT;
  test.skip(!executable || !port, 'Requires an explicitly configured Proxmark3 HF fixture');
  test.skip(process.env.CHAMELEON_BLE_TEST_READER_POSITIVE !== '1', 'Requires an aligned HF simulation');
  test.setTimeout(180000);
  page.setDefaultTimeout(15000);
  await mkdir(testInfo.outputDir, { recursive: true });
  const info = async () => await (await request.get(deviceURL + '/api/device')).json();
  const before = await info();
  expect(before.busy).toBe(false);
  expect(before.reader).toBe(false);
  const metadata = [];
  for (let slot = 0; slot < 8; slot++) {
    metadata.push(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body());
  }
  const checkChooser = await physicalChooser(page);
  const leakedHttp = [];
  await page.route('**/api/**', (route) => {
    leakedHttp.push(route.request().url());
    return route.abort();
  });
  let stopSimulation;
  let session = 0;
  let qualified = false;
  const usb = [];
  let captured;
  let uiStatus;
  try {
    stopSimulation = await startSimulation(
      executable,
      port,
      'hf 14a sim -t 1 -u 11223344',
      testInfo.outputPath('simulation.log')
    );
    const opened = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
    expect(opened.status()).toBe(200);
    session = (await opened.json()).session;
    for (let round = 0; round < 3; round++) {
      const response = await request.post(`${deviceURL}/api/command/2000?id=${session}`, {
        data: Buffer.alloc(0)
      });
      expect(response.status()).toBe(200);
      const bytes = [...(await response.body())];
      expect(bytes).toEqual([0, 0, 4, ...uid, 4, 0, 8, 0]);
      usb.push(bytes);
    }
    expect(
      (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
    ).toBe(200);
    session = 0;
    await page.goto('./#device');
    await page.getByRole('button', { name: 'Connect Bluetooth', exact: true }).click();
    await expect(page.locator('#device-info')).toContainText(before.chip);
    await page.locator('[data-view-target="scan"]').click();
    await page.locator('#scan').click();
    await expect(page.locator('#studio-document')).toBeVisible();
    await expect(page.locator('#stop')).toBeHidden({ timeout: 60000 });
    let exportButton = page.locator('#export-json');
    if (!(await exportButton.isVisible())) {
      await page.locator('#export-menu summary').click();
      exportButton = page.locator('#export-document');
    }
    const download = page.waitForEvent('download');
    await exportButton.click();
    const stream = await (await download).createReadStream();
    const parts = [];
    for await (const part of stream) {
      parts.push(part);
    }
    captured = JSON.parse(Buffer.concat(parts).toString()).document;
    expect(captured.card).toMatchObject({ uid, atqa: [0, 4], sak: 8, ats: [] });
    expect(captured.memory).toEqual([]);
    uiStatus = await page.locator('#status').textContent();
    expect(leakedHttp).toEqual([]);
    checkChooser();
    qualified = true;
  } finally {
    await page.close();
    if (session) {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${session}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    if (stopSimulation) {
      await stopSimulation();
    }
    await expect.poll(async () => (await info()).busy).toBe(false);
    const negativeLease = await request.post(deviceURL + '/api/session', { data: Buffer.alloc(0) });
    expect(negativeLease.status()).toBe(200);
    const negativeSession = (await negativeLease.json()).session;
    try {
      const absent = await request.post(`${deviceURL}/api/command/2000?id=${negativeSession}`, {
        data: Buffer.alloc(0)
      });
      expect(absent.status()).toBe(200);
      expect([...(await absent.body())]).toEqual([0, 1]);
    } finally {
      expect(
        (await request.post(`${deviceURL}/api/end?id=${negativeSession}`, { data: Buffer.alloc(0) })).status()
      ).toBe(200);
    }
    const after = await info();
    for (const key of ['chip', 'git', 'settings', 'reader', 'active_slot']) {
      expect(after[key], key).toEqual(before[key]);
    }
    for (let slot = 0; slot < 8; slot++) {
      expect(await (await request.get(`${deviceURL}/api/slot/${slot}`)).body()).toEqual(metadata[slot]);
    }
    await testInfo.attach('physical-reader-positive-evidence', {
      body: JSON.stringify(
        {
          qualified,
          preserved: true,
          chip: before.chip,
          firmware: before.git,
          usb,
          card: captured?.card,
          uiStatus,
          pm3Cleanup: JSON.parse(await readFile(testInfo.outputPath('simulation.log.cleanup.json'), 'utf8')),
          simulationAbsentAfterStop: true,
          httpFallbackRequests: leakedHttp,
          physicalWrites: false,
          memoryReadQualified: false,
          fixture: 'PM3 NFC-A simulation'
        },
        null,
        2
      ),
      contentType: 'application/json'
    });
  }
});
