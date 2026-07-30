import { defineConfig } from 'vitest/config';

// Each test file spawns its own anoa-browser on the same port triplet
// (ANOA_PORT..+2). Files must therefore never run concurrently — a second
// instance would fail to bind and the suite would silently talk to the
// wrong browser (e.g. auth suites hitting a no-auth instance).
export default defineConfig({
  test: {
    fileParallelism: false,
  },
});
