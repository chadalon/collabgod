// SPDX-License-Identifier: GPL-3.0-or-later

// The examples the C++ self-test checks against Tracktion, checked here against the
// reference semantics. Together they make the TypeScript reference and the engine agree.

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { applyCommands, type Command } from '../src/commands';
import { emptyDoc, type EngineDoc, firstDifference } from '../src/document';

const dir = join(import.meta.dirname, '..', '..', 'contract', 'examples');
const samples = '/any/folder';

describe('contract examples', () => {
  const files = readdirSync(dir).filter((f) => f.endsWith('.json')).sort();

  it('there are examples', () => expect(files.length).toBeGreaterThan(0));

  for (const file of files) {
    it(file, () => {
      const example = JSON.parse(readFileSync(join(dir, file), 'utf8').replaceAll('${samples}', samples)) as {
        commands: Command[];
        readback: EngineDoc;
        failedAt?: number;
      };
      const outcome = applyCommands(emptyDoc, example.commands);
      expect(outcome.failedAt).toBe(example.failedAt);
      expect(firstDifference(outcome.doc, example.readback)).toBeUndefined();
    });
  }
});
