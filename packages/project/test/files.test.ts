// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { loadProject, PROJECT_VERSION, saveProject } from '../src';
import v1Small from './files/v1-small.collabgod?raw';
import { loaded, smallProject, stateOf } from './helpers';

/**
 * Files saved by earlier versions of the format, kept for good. Every one must
 * open in every later version of the app. When the format changes, keep these
 * as they are and add the new version's example beside them.
 */
describe('example files', () => {
  it('version 1: a small project opens with everything it held', () => {
    const project = loaded(loadProject(v1Small));
    expect(stateOf(project)).toBe(stateOf(smallProject()));
    expect(project.seq).toBe(11);
    expect(project.session.clips.map((clip) => [clip.id, clip.start, clip.length, clip.offset])).toEqual([
      ['k1', 0, 960, 0],
      ['k2', 1920, 960, 0],
      ['k3', 370, 3470, 0.25],
    ]);
    expect(project.history?.redo).toHaveLength(1);
  });

  it('version 1 is still what this code writes, until the version is raised', () => {
    // If this fails, the format changed: raise PROJECT_VERSION, add a migration and a new example (see migrate.ts).
    expect(PROJECT_VERSION).toBe(1);
    expect(saveProject(smallProject())).toBe(v1Small);
  });
});
