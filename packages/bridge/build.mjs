// Generates the app's C++ side of the contract into dist/cpp, where the app's
// CMakeLists.txt finds it. `npm run build` at the repository root runs this before
// building the app. Nothing it writes is committed.
//
//   node build.mjs

import { build } from 'esbuild';
import { mkdir, rm, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const out = new URL('./dist/cpp/', import.meta.url);

// The generator and the contract are TypeScript; bundle them for Node and load the result.
const bundled = await build({
  // A path, not URL.pathname, which is /D:/... on Windows.
  entryPoints: [fileURLToPath(new URL('./src/generate.ts', import.meta.url))],
  bundle: true,
  format: 'esm',
  platform: 'neutral',
  write: false,
  logLevel: 'warning',
});
const code = bundled.outputFiles[0].text;
const { generateCpp, contract } = await import(`data:text/javascript;base64,${Buffer.from(code).toString('base64')}`);

await rm(out, { recursive: true, force: true });
await mkdir(out, { recursive: true });

for (const file of generateCpp(contract)) {
  const target = new URL(file.name, out);
  await writeFile(target, file.text);
  console.log(`Wrote ${fileURLToPath(target)}`);
}

// A stamp CMake can check, so a configure with no generated headers fails with a clear message.
await writeFile(new URL('contract-version.txt', out), `${contract.version}\n`);
