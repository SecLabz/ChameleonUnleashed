import { defineConfig, devices } from '@playwright/test';
import hosted from './playwright.hosted.config.mjs';

// Browser emulation exercises touch/layout/storage, never native iOS BLE.
export default defineConfig({
  ...hosted,
  outputDir: './test-results-mobile',
  projects: [
    { name: 'iphone-webkit', use: devices['iPhone 13'] },
    {
      name: 'webkit-touch-320',
      use: { ...devices['iPhone 13'], viewport: { width: 320, height: 568 } }
    }
  ]
});
