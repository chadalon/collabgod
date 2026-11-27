import { defineConfig } from 'vitest/config';

// FC_SCALE multiplies the property tests' random cases (see ../session/test/helpers.ts), so it stretches their time limit too.
const scale = Number(
  (globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env['FC_SCALE'] ?? 1,
);

export default defineConfig({
  // 30 seconds a test: the longest property test takes about 4 here, and shared CI runners
  // (Windows especially) can be several times slower. Timing budgets are asserted in the tests.
  test: { include: ['test/**/*.test.ts'], testTimeout: 30_000 * Math.max(1, scale) },
});
