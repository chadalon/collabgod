// Tests for check-no-spike-imports.mjs. Run by `npm test` with Node's own test runner.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { checkRepository, findSpikeReferences } from './check-no-spike-imports.mjs';

test('finds TypeScript imports from a spike', () => {
  const lines = (text) => findSpikeReferences(text, 'c-like').map((hit) => hit.line);
  assert.deepEqual(lines(`import { Replica } from '../../spikes/session-model/src/index.ts';`), [1]);
  assert.deepEqual(lines(`const m = await import("../spikes/relay/src/server.ts");`), [1]);
  assert.deepEqual(lines(`export * from '../../../spikes/relay/src/index.ts';`), [1]);
  assert.deepEqual(lines(`const x = require('spikes/relay');`), [1]);
});

test('finds C++ includes and CMake that build a spike', () => {
  assert.equal(findSpikeReferences(`#include "../../spikes/engine-toolchain/src/ToneCheck.h"`, 'c-like').length, 1);
  assert.equal(findSpikeReferences(`include(../spikes/engine-toolchain/cmake/EngineDependencies.cmake)`, 'cmake').length, 1);
  assert.equal(findSpikeReferences(`add_subdirectory(\${PROJECT_SOURCE_DIR}/spikes/relay)`, 'cmake').length, 1);
  assert.equal(findSpikeReferences(`target_sources(app PRIVATE "spikes\\\\webview-ui\\\\src\\\\Bridge.cpp")`, 'cmake').length, 1);
});

test('finds a package or TypeScript config that points at a spike', () => {
  assert.equal(findSpikeReferences(`{ "dependencies": { "relay": "file:../../spikes/relay" } }`, 'plain').length, 1);
  assert.equal(findSpikeReferences(`{ "references": [{ "path": "../spikes/session-model" }] }`, 'plain').length, 1);
});

test('ignores comments that cite a spike, and names that only end in "spikes"', () => {
  assert.deepEqual(findSpikeReferences(`// From spikes/engine-projection, finding 6.\nconst a = 1;`, 'c-like'), []);
  assert.deepEqual(findSpikeReferences(`/* see\n spikes/relay/README.md */\nconst a = 1;`, 'c-like'), []);
  assert.deepEqual(findSpikeReferences(`# engine-toolchain: spikes/engine-toolchain README finding 11`, 'cmake'), []);
  assert.deepEqual(findSpikeReferences(`set(x 1) # spikes/webview-ui proved it`, 'cmake'), []);
  assert.deepEqual(findSpikeReferences(`import { a } from './myspikes/a.ts';`, 'c-like'), []);
  assert.deepEqual(findSpikeReferences(`const url = 'https://example.com/a//b';`, 'c-like'), []);
});

test('a comment marker inside a string does not hide what follows it', () => {
  assert.equal(findSpikeReferences(`const u = "http://x"; import a from '../spikes/a.ts';`, 'c-like').length, 1);
  assert.equal(findSpikeReferences(`message(STATUS "# not a comment") include(spikes/x.cmake)`, 'cmake').length, 1);
});

test('walks product folders, skips dependencies and build output, and reports file and line', async () => {
  const root = await mkdtemp(join(tmpdir(), 'spike-check-'));
  try {
    await mkdir(join(root, 'packages', 'a', 'src'), { recursive: true });
    await mkdir(join(root, 'packages', 'a', 'node_modules', 'x'), { recursive: true });
    await mkdir(join(root, 'app', 'src'), { recursive: true });
    await writeFile(join(root, 'packages', 'a', 'src', 'ok.ts'), `// spikes/relay showed this\nexport const a = 1;\n`);
    await writeFile(join(root, 'packages', 'a', 'src', 'bad.ts'), `export {};\nimport '../../../spikes/relay/src/a.ts';\n`);
    await writeFile(join(root, 'packages', 'a', 'node_modules', 'x', 'index.js'), `require('../spikes/x');\n`);
    await writeFile(join(root, 'app', 'src', 'Main.cpp'), `#include "../../spikes/a.h"\n`);
    await writeFile(join(root, 'CMakeLists.txt'), `add_subdirectory(app)\n`);

    const problems = await checkRepository(root, ['app', 'packages', 'CMakeLists.txt', 'missing']);
    assert.deepEqual(
      problems.map((p) => `${p.file}:${p.line}`).sort(),
      ['app/src/Main.cpp:1', 'packages/a/src/bad.ts:2'],
    );
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('the repository itself is clean', async () => {
  assert.deepEqual(await checkRepository(), []);
});
