// SPDX-License-Identifier: GPL-3.0-or-later

// The whole path in memory: random edits through the session model, projected, diffed
// and applied to an engine that follows the reference semantics.

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { emptyState, type SessionState } from '../../../session-model/src/model';
import { applyCommands } from '../src/commands';
import { firstDifference, project } from '../src/document';
import { RandomEditor, seededRandom } from '../src/edits';
import { FakeEngine, Projector } from '../src/projector';

const files = new Map([
  ['h1', '/s/1.wav'],
  ['h2', '/s/2.wav'],
]);
const resolve = (hash: string) => files.get(hash);

describe('Projector', () => {
  it('property: after every edit the engine holds exactly the projection of the model', async () => {
    await fc.assert(
      fc.asyncProperty(fc.integer(), fc.integer({ min: 1, max: 80 }), async (seed, steps) => {
        const engine = new FakeEngine(applyCommands);
        const projector = new Projector(engine, resolve);
        const editor = new RandomEditor(seededRandom(seed), { sampleHashes: ['h1', 'h2'], spanTicks: 19200, maxTracks: 6 });
        let state: SessionState = emptyState;
        for (let i = 0; i < steps; i++) {
          state = editor.applyEdit(state, editor.next(state)).state;
          const result = await projector.update(state);
          expect(result.rebuilt).toBe(false);
          expect(firstDifference(engine.doc, project(state, resolve).doc)).toBeUndefined();
        }
      }),
      { numRuns: 300 },
    );
  });

  it('only the difference is sent', async () => {
    const engine = new FakeEngine(applyCommands);
    const projector = new Projector(engine, resolve);
    const editor = new RandomEditor(seededRandom(1), { sampleHashes: ['h1'], spanTicks: 19200, maxTracks: 4 });
    let state: SessionState = emptyState;
    for (let i = 0; i < 30; i++) state = editor.applyEdit(state, editor.next(state)).state;
    await projector.update(state);
    const clip = Object.values(state.clips)[0];
    if (!clip) throw new Error('no clip');
    state = editor.applyEdit(state, { op: { type: 'clip.move', id: clip.id, trackId: clip.trackId, start: clip.start + 1 }, description: '' }).state;
    expect((await projector.update(state)).commands).toBe(1);
  });

  it('a refused command makes it rebuild from what the engine reports', async () => {
    const engine = new FakeEngine(applyCommands);
    const projector = new Projector(engine, resolve);
    const editor = new RandomEditor(seededRandom(2), { sampleHashes: ['h1', 'h2'], spanTicks: 19200, maxTracks: 4 });
    let state: SessionState = emptyState;
    for (let i = 0; i < 20; i++) state = editor.applyEdit(state, editor.next(state)).state;
    await projector.update(state);

    // Someone else changes the engine behind the projector's back.
    engine.doc = { tracks: [] };
    state = editor.applyEdit(state, { op: { type: 'track.add', track: { id: 'new', name: 'new' }, index: 0 }, description: '' }).state;
    const clip = Object.values(state.clips)[0];
    if (clip) state = editor.applyEdit(state, { op: { type: 'clip.move', id: clip.id, trackId: clip.trackId, start: clip.start + 7 }, description: '' }).state;

    const result = await projector.update(state);
    expect(result.rebuilt).toBe(true);
    expect(await projector.verify()).toBeUndefined();
  });

  it('a silently lost command is found by verify and fixed by rebuild', async () => {
    const engine = new FakeEngine(applyCommands);
    const projector = new Projector(engine, resolve);
    const editor = new RandomEditor(seededRandom(3), { sampleHashes: ['h1', 'h2'], spanTicks: 19200, maxTracks: 4 });
    let state: SessionState = emptyState;
    for (let i = 0; i < 20; i++) state = editor.applyEdit(state, editor.next(state)).state;
    await projector.update(state);

    const clip = Object.values(state.clips)[0];
    if (!clip) throw new Error('no clip');
    engine.dropNext = true;
    state = editor.applyEdit(state, { op: { type: 'clip.move', id: clip.id, trackId: clip.trackId, start: clip.start + 9 }, description: '' }).state;
    await projector.update(state);

    expect(await projector.verify()).toMatch(new RegExp(`clip ${clip.id}`));
    await projector.rebuild();
    expect(await projector.verify()).toBeUndefined();
  });

  it('rebuilds from scratch to the same result', async () => {
    const engine = new FakeEngine(applyCommands);
    const projector = new Projector(engine, resolve);
    const editor = new RandomEditor(seededRandom(4), { sampleHashes: ['h1', 'h2'], spanTicks: 19200, maxTracks: 5 });
    let state: SessionState = emptyState;
    for (let i = 0; i < 60; i++) state = editor.applyEdit(state, editor.next(state)).state;
    await projector.update(state);
    const before = engine.doc;
    await projector.rebuildFromScratch();
    expect(firstDifference(engine.doc, before)).toBeUndefined();
  });
});
