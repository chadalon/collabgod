// Builds everything: the TypeScript packages, then the C++ app.
//
//   npm run build                     everything, Release
//   npm run build:ts                  just the TypeScript packages
//   npm run build:app                 just the C++ app
//   npm run build -- --jobs 2         set the parallel compile jobs. The default fits
//                                     this machine's memory: 2 on an 8 GB machine.
//                                     COLLABGOD_JOBS=2 does the same.
//   npm run build -- --config Debug   a Debug build of the app
//
// The first build of the app downloads JUCE and Tracktion Engine (about 250 MB) and
// takes several minutes; later builds compile only what changed. See README.md.

import { ROOT, defaultJobs, ensureNodeModules, fail, isMain, npm, parseOptions, run, step } from './lib.mjs';
import { resolve } from 'node:path';

const USAGE = `Usage: npm run build -- [--only ts|app] [--config Debug|Release|RelWithDebInfo] [--jobs N] [--build-dir DIR]`;

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

  if (options.only !== 'app') {
    ensureNodeModules();
    step('TypeScript: type check');
    npm(['run', 'typecheck', '--workspaces', '--if-present']);
    step('TypeScript: build');
    npm(['run', 'build', '--workspaces', '--if-present']);
  }

  if (options.only !== 'ts') {
    const buildDir = resolve(ROOT, options.buildDir);
    const jobs = options.jobs ?? defaultJobs();
    step(`C++ app: configure (${options.config})`);
    run('cmake', ['-S', ROOT, '-B', buildDir, `-DCMAKE_BUILD_TYPE=${options.config}`]);
    step(`C++ app: build with ${jobs} parallel job${jobs === 1 ? '' : 's'}${options.jobs ? '' : ' (chosen from this machine\'s memory; --jobs N or COLLABGOD_JOBS=N to change)'}`);
    run('cmake', ['--build', buildDir, '--config', options.config, '--parallel', String(jobs)]);
  }

  step('Build finished');
}

if (isMain(import.meta.url)) main(process.argv.slice(2));
