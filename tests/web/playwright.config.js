// SPDX-License-Identifier: MIT
// Browser tests for web/index.html (installer) and the setup/diagnostics pages served
// by the firmware, against a simulated board. Run: npm test (in tests/web).
const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: '.',
  testMatch: '*.spec.js',
  timeout: 30000,
  reporter: process.env.CI ? [['list'], ['github']] : 'list',
  use: {
    viewport: { width: 390, height: 900 },
    trace: 'off',
    // Use a preinstalled Chromium when given (e.g. in a sandbox without downloads).
    launchOptions: process.env.PW_CHROMIUM_PATH ? { executablePath: process.env.PW_CHROMIUM_PATH } : {},
  },
});
