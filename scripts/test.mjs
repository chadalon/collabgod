// Runs every test: the build scripts' own, the TypeScript packages', and the C++
// app's (with ctest). Build first with `npm run build`.
//
//   npm test                      everything
//   npm run test:ts               just the TypeScript packages (no C++ build needed)
//   npm run test:app              just the C++ app's tests
//   npm test -- --config Debug    test a Debug build

import { readdirSync, existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { ROOT, ensureNodeModules, fail, hasCommand, isMain, npm, parseOptions, run, step } from './lib.mjs';

const USAGE = `Usage: npm test -- [--only ts|app] [--config Debug|Release|RelWithDebInfo] [--build-dir DIR]`;

export function main(argv) {
  let options;
  try {
    options = parseOptions(argv);
  } catch (error) {
    fail(`${error.message}\n${USAGE}`);
  }
  if (options.help) {
    console.log(USAGE);
    return;
  }

  // Includes the check that no product code imports a spike.
  step('Build scripts and repository checks');
  const scriptTests = readdirSync(join(ROOT, 'scripts'))
    .filter((name) => name.endsWith('.test.mjs'))
    .map((name) => join('scripts', name));
  run(process.execPath, ['--test', ...scriptTests]);

  if (options.only !== 'app') {
    ensureNodeModules();
    step('TypeScript packages');
    npm(['test', '--workspaces', '--if-present']);
  }

  if (options.only !== 'ts') {
    const buildDir = resolve(ROOT, options.buildDir);
    if (!existsSync(join(buildDir, 'CTestTestfile.cmake'))) {
      fail(`No C++ build in ${buildDir}. Run \`npm run build\` first, or \`npm test -- --only ts\` for the TypeScript tests alone.`);
    }
    step(`C++ app (${options.config})`);
    const ctest = ['--test-dir', buildDir, '-C', options.config, '--output-on-failure'];
    // JUCE's GUI app wants a display even when it opens no window. Linux is not a
    // product platform, but it is where cloud sessions build.
    if (process.platform === 'linux' && !process.env.DISPLAY && hasCommand('xvfb-run', ['--help'])) {
      run('xvfb-run', ['--auto-servernum', 'ctest', ...ctest]);
    } else {
      run('ctest', ctest);
    }
  }

  step('All tests passed');
}

if (isMain(import.meta.url)) main(process.argv.slice(2));
