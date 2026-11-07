import { defineConfig, devices } from '@playwright/test';

const port = Number(process.env.CHAMELEON_WEB_TEST_PORT || 8080);
if (!Number.isInteger(port) || port < 1024 || port > 65535) {
  throw new Error('Invalid CHAMELEON_WEB_TEST_PORT');
}
const baseURL = `http://127.0.0.1:${port}`;

export default defineConfig({
  testDir: './tests/browser',
  workers: 1,
  use: { baseURL, trace: 'retain-on-failure' },
  projects: [
    { name: 'chromium', use: devices['Desktop Chrome'] },
    { name: 'webkit', use: devices['Desktop Safari'] }
  ],
  webServer: { command: `python3 tests/serve.py --port ${port}`, url: baseURL, reuseExistingServer: false }
});
