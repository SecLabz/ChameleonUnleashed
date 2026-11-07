import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: './tests/hosted',
  outputDir: './test-results-hosted',
  workers: 1,
  timeout: 60000,
  expect: { timeout: 20000 },
  use: {
    baseURL: 'http://127.0.0.1:8085/project/ChameleonUnleashed/app/',
    trace: 'retain-on-failure'
  },
  projects: [
    { name: 'chromium', use: devices['Desktop Chrome'] },
    { name: 'webkit', use: devices['Desktop Safari'] }
  ],
  webServer: {
    command: 'python3 tests/serve_hosted.py',
    url: 'http://127.0.0.1:8085/project/ChameleonUnleashed/app/',
    reuseExistingServer: false
  }
});
