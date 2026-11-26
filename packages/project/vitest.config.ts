import { defineConfig } from 'vitest/config';

// FC_SCALE multiplies the property tests' random cases (see ../session/test/helpers.ts), so it stretches their time limit too.
const scale = Number(
  (globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env['FC_SCALE'] ?? 1,
);

export default defineConfig({
  test: { include: ['test/**/*.test.ts'], testTimeout: 10_000 * Math.max(1, scale) },
});
