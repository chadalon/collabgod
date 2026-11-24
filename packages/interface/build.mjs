// Bundles the interface into dist/, which the app will serve to its web view (#26).
//   node build.mjs           build once
//   node build.mjs --watch   rebuild on change

import { build, context } from 'esbuild';
import { copyFile, mkdir, rm } from 'node:fs/promises';

const dist = new URL('./dist/', import.meta.url);
const watch = process.argv.includes('--watch');

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });
await copyFile(new URL('./index.html', import.meta.url), new URL('index.html', dist));

const options = {
  entryPoints: ['src/main.ts'],
  bundle: true,
  format: 'esm',
  // WKWebView on macOS 12, the oldest macOS the app supports (#145), is at least
  // Safari 15's WebKit. WebView2 on Windows is current Chromium.
  target: ['safari15', 'chrome120'],
  sourcemap: true,
  outfile: 'dist/app.js',
  logLevel: 'info',
};

if (watch) {
  const ctx = await context(options);
  await ctx.watch();
} else {
  await build(options);
}
