import { defineConfig } from '@playwright/test';

const hosted = process.env.CHAMELEON_BLE_TEST_HOSTED === '1';
const baseURL = hosted ? 'http://127.0.0.1:8085/project/ChameleonUnleashed/app/' : 'http://127.0.0.1:8083';

// Explicit opt-in: this configuration contacts the physical USB/BLE Ultra.
export default defineConfig({
  testDir: './tests/hardware',
  outputDir: './test-results-hardware',
  reporter: [['list'], ['json', { outputFile: './test-results-hardware/report.json' }]],
  workers: 1,
  timeout: 90000,
  expect: { timeout: 15000 },
  use: {
    baseURL,
    trace: 'retain-on-failure',
    channel: 'chromium',
    launchOptions: { args: ['--enable-features=WebBluetooth,WebBluetoothNewPermissionsBackend'] }
  },
  webServer: {
    command: hosted ? 'python3 tests/serve_hosted.py' : '../../dev serve --port 8083',
    url: baseURL,
    timeout: 120000,
    reuseExistingServer: false
  }
});
