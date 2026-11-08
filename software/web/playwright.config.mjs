import { availableParallelism } from 'node:os';
import { defineConfig, devices } from '@playwright/test';

// Each worker drives its own simulator on port + its parallel index: the
// simulated card lives in one serve.py process and tests reset it freely.
const port = Number(process.env.CHAMELEON_WEB_TEST_PORT || 8080);
const workers = Number(
  process.env.CHAMELEON_WEB_TEST_WORKERS ?? Math.min(4, Math.max(1, Math.floor(availableParallelism() / 2)))
);
if (!Number.isInteger(workers) || workers < 1) {
  throw new Error('Invalid CHAMELEON_WEB_TEST_WORKERS');
}
if (!Number.isInteger(port) || port < 1024 || port + workers > 65536) {
  throw new Error('Invalid CHAMELEON_WEB_TEST_PORT');
}
const server = (index) => `http://127.0.0.1:${port + index}`;

export default defineConfig({
  testDir: './tests/browser',
  workers,
  // A broken local change stops early instead of waiting out every timeout;
  // CI still reports every failure.
  maxFailures: process.env.CI ? 0 : 10,
  use: { baseURL: server(Number(process.env.TEST_PARALLEL_INDEX || 0)), trace: 'retain-on-failure' },
  projects: [
    { name: 'chromium', use: devices['Desktop Chrome'] },
    { name: 'webkit', use: devices['Desktop Safari'] }
  ],
  webServer: Array.from({ length: workers }, (_, index) => ({
    command: `python3 tests/serve.py --port ${port + index}`,
    url: server(index),
    reuseExistingServer: false
  }))
});
