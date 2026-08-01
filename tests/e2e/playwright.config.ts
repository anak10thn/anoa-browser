import { defineConfig } from '@playwright/test';

const PORT = parseInt(process.env.ANOA_PORT ?? '9222', 10);

export default defineConfig({
  testMatch: ['**/playwright.test.ts'],
  timeout: 30000,
  use: {
    // All tests connect to the already-running anoa binary.
    // The binary must be started externally before running these tests.
    baseURL: `http://localhost:${PORT}`,
  },
  reporter: [['list']],
});
