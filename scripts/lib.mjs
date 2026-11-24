// Shared helpers for the root build and test scripts. Plain Node, no dependencies,
// so it runs before `npm ci` has installed anything.

import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { cpus, totalmem } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = fileURLToPath(new URL('..', import.meta.url));

/** True when the module at `url` is the script Node was started with. */
export function isMain(url) {
  if (!process.argv[1]) return false;
  const a = resolve(fileURLToPath(url));
  const b = resolve(process.argv[1]);
  return process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;
}

/**
 * Parallel compile jobs that fit in this machine's memory. A JUCE and Tracktion
 * Engine compile job needs up to about 3.5 GB: on a 7 GB machine four jobs got the
 * compiler killed and two were fine, and on an 8 GB Mac four made it stall and
 * restart (engine-toolchain README findings 6 and 11). So: one job per 3.5 GB,
 * at least one, at most one per core. 8 GB gives 2.
 */
export function defaultJobs(memoryBytes = totalmem(), cores = cpus().length) {
  const byMemory = Math.floor(memoryBytes / 1024 ** 3 / 3.5);
  return Math.max(1, Math.min(cores, byMemory));
}

const CONFIGS = ['Debug', 'Release', 'RelWithDebInfo'];

/**
 * Parses the options both scripts share.
 *   --only ts|app      just the TypeScript packages, or just the C++ app
 *   --config <name>    Debug, Release (default) or RelWithDebInfo
 *   --jobs <n>         parallel compile jobs (default: COLLABGOD_JOBS, or what fits in memory)
 *   --build-dir <dir>  CMake build folder (default: build)
 */
export function parseOptions(argv, env = process.env) {
  const options = { only: null, config: 'Release', jobs: null, buildDir: 'build', help: false };
  if (env.COLLABGOD_JOBS !== undefined && env.COLLABGOD_JOBS !== '') {
    options.jobs = parseJobs(env.COLLABGOD_JOBS, 'COLLABGOD_JOBS');
  }
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    const value = () => {
      const v = argv[i + 1];
      if (v === undefined || v.startsWith('--')) throw new Error(`${arg} needs a value`);
      i += 1;
      return v;
    };
    switch (arg) {
      case '--only': {
        const v = value();
        if (v !== 'ts' && v !== 'app') throw new Error(`--only takes ts or app, not ${v}`);
        options.only = v;
        break;
      }
      case '--config': {
        const v = value();
        const match = CONFIGS.find((c) => c.toLowerCase() === v.toLowerCase());
        if (!match) throw new Error(`--config takes ${CONFIGS.join(', ')}, not ${v}`);
        options.config = match;
        break;
      }
      case '--jobs':
        options.jobs = parseJobs(value(), '--jobs');
        break;
      case '--build-dir':
        options.buildDir = value();
        break;
      case '--help':
      case '-h':
        options.help = true;
        break;
      default:
        throw new Error(`Unknown option ${arg}`);
    }
  }
  return options;
}

function parseJobs(text, name) {
  const v = Number(text);
  if (!Number.isInteger(v) || v < 1) throw new Error(`${name} takes a whole number of 1 or more, not ${text}`);
  return v;
}

export function step(title) {
  console.log(`\n=== ${title}`);
}

/** Runs a command in the repository root, showing its output. Exits on failure. */
export function run(command, args, { cwd = ROOT, env = process.env, shell = false } = {}) {
  console.log(`> ${[command, ...args].join(' ')}`);
  const result = spawnSync(command, args, { cwd, env, stdio: 'inherit', shell });
  if (result.error) {
    if (result.error.code === 'ENOENT') {
      fail(`${command} was not found. Is it installed and on your PATH? See README.md, "Building".`);
    }
    fail(String(result.error));
  }
  if (result.status !== 0) fail(`${command} failed with exit code ${result.status ?? result.signal}`);
}

/** The npm that started this script, or the one installed beside this Node. */
function npmCli() {
  const candidates = [
    process.env.npm_execpath,
    join(dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js'), // Windows
    join(dirname(process.execPath), '..', 'lib', 'node_modules', 'npm', 'bin', 'npm-cli.js'), // macOS, Linux
  ];
  return candidates.find((c) => c && /npm-cli\.c?js$/.test(c) && existsSync(c)) ?? null;
}

/** Runs npm in the repository root. */
export function npm(args) {
  const cli = npmCli();
  if (cli) run(process.execPath, [cli, ...args]);
  // Node refuses to start npm.cmd without a shell on Windows.
  else run('npm', args, { shell: process.platform === 'win32' });
}

/** Installs the TypeScript dependencies from the lockfile if they are not there yet. */
export function ensureNodeModules() {
  if (!existsSync(resolve(ROOT, 'node_modules', '.package-lock.json'))) {
    step('Installing TypeScript dependencies (npm ci)');
    npm(['ci']);
  }
}

export function fail(message) {
  console.error(`\n${message}`);
  process.exit(1);
}

/** True when `command` can be started. */
export function hasCommand(command, args = ['--version']) {
  const result = spawnSync(command, args, { stdio: 'ignore' });
  return !result.error;
}
