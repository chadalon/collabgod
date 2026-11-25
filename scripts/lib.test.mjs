// Tests for the build scripts' shared helpers. Run by `npm test`.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { defaultJobs, parseOptions } from './lib.mjs';

const GB = 1024 ** 3;

test('parallel jobs follow memory: 2 on an 8 GB machine, whatever its cores', () => {
  assert.equal(defaultJobs(8 * GB, 4), 2); // Chad's Intel MacBook: 4 jobs stalled it
  assert.equal(defaultJobs(7 * GB, 2), 2); // 2-core 7 GB cloud machine: 4 jobs were killed, 2 fine
  assert.equal(defaultJobs(16 * GB, 4), 4);
  assert.equal(defaultJobs(64 * GB, 16), 16);
  assert.equal(defaultJobs(64 * GB, 4), 4); // never more than the cores
  assert.equal(defaultJobs(2 * GB, 8), 1); // always at least one
});

test('options: defaults', () => {
  assert.deepEqual(parseOptions([], {}), { only: null, config: 'Release', jobs: null, buildDir: 'build', help: false });
});

test('options: every flag', () => {
  assert.deepEqual(parseOptions(['--only', 'ts', '--config', 'debug', '--jobs', '2', '--build-dir', 'build-debug'], {}), {
    only: 'ts',
    config: 'Debug',
    jobs: 2,
    buildDir: 'build-debug',
    help: false,
  });
  assert.equal(parseOptions(['--only', 'app'], {}).only, 'app');
  assert.equal(parseOptions(['-h'], {}).help, true);
});

test('options: COLLABGOD_JOBS sets the jobs, and --jobs beats it', () => {
  assert.equal(parseOptions([], { COLLABGOD_JOBS: '2' }).jobs, 2);
  assert.equal(parseOptions(['--jobs', '3'], { COLLABGOD_JOBS: '2' }).jobs, 3);
  assert.equal(parseOptions([], { COLLABGOD_JOBS: '' }).jobs, null);
  assert.throws(() => parseOptions([], { COLLABGOD_JOBS: 'lots' }), /COLLABGOD_JOBS takes/);
});

test('options: refuses what it does not understand', () => {
  assert.throws(() => parseOptions(['--only', 'cpp'], {}), /--only takes ts or app/);
  assert.throws(() => parseOptions(['--jobs', '0'], {}), /whole number/);
  assert.throws(() => parseOptions(['--jobs', 'two'], {}), /whole number/);
  assert.throws(() => parseOptions(['--config', 'Fast'], {}), /--config takes/);
  assert.throws(() => parseOptions(['--jobs'], {}), /needs a value/);
  assert.throws(() => parseOptions(['--jobs', '--only', 'ts'], {}), /needs a value/);
  assert.throws(() => parseOptions(['--parallel', '2'], {}), /Unknown option --parallel/);
});
