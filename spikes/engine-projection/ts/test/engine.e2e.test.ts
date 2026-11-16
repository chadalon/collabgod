// SPDX-License-Identifier: GPL-3.0-or-later

// The whole path through the real engine: the session model, the projection, the
// engine process and Tracktion Engine, with a simulated sound card. Runs when
// ENGINE_PROJECTION_BIN points at a built engine_projection; CI builds it first.

import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import fc from 'fast-check';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { emptyState, type SessionState } from '../../../session-model/src/model';
import { project } from '../src/document';
import { RandomEditor, seededRandom } from '../src/edits';
import { EngineProcess } from '../src/engine';
import { Projector } from '../src/projector';
import { generatedLibrary } from '../src/samples';

const binary = process.env['ENGINE_PROJECTION_BIN'];

describe.skipIf(!binary)('through the real engine', () => {
  const library = generatedLibrary(mkdtempSync(join(tmpdir(), 'collabgod-e2e-')));
  const resolve = (hash: string) => library.files.get(hash);
  let engine: EngineProcess;

  beforeAll(async () => {
    engine = new EngineProcess(binary as string, { simulatedAudio: true });
    await engine.hello();
    await engine.waitUntilSettled();
  });

  afterAll(async () => {
    await engine?.quit();
  });

  it('property: after every batch of edits, what Tracktion holds is the projection of the model', async () => {
    await fc.assert(
      fc.asyncProperty(fc.integer(), async (seed) => {
        await engine.apply([{ op: 'reset' }]);
        const projector = new Projector(engine, resolve);
        const editor = new RandomEditor(seededRandom(seed), { sampleHashes: library.hashes, spanTicks: 8 * 1920, maxTracks: 6 });
        let state: SessionState = emptyState;
        for (let batch = 0; batch < 8; batch++) {
          for (let i = 0; i < 5; i++) state = editor.applyEdit(state, editor.next(state)).state;
          const result = await projector.update(state);
          expect(result.rebuilt).toBe(false);
          expect(await projector.verify()).toBeUndefined();
        }
      }),
      { numRuns: 25 },
    );
  });

  it('rebuilds a fresh engine from the model', async () => {
    await engine.apply([{ op: 'reset' }]);
    const editor = new RandomEditor(seededRandom(5), { sampleHashes: library.hashes, spanTicks: 8 * 1920, maxTracks: 8 });
    let state: SessionState = emptyState;
    for (let i = 0; i < 150; i++) state = editor.applyEdit(state, editor.next(state)).state;
    await new Projector(engine, resolve).update(state);

    // A second engine process that has never seen this session.
    const fresh = new EngineProcess(binary as string, { simulatedAudio: true });
    try {
      await fresh.waitUntilSettled();
      const projector = new Projector(fresh, resolve);
      await projector.update(state);
      expect(await projector.verify()).toBeUndefined();
      expect(await fresh.readback()).toEqual(await engine.readback());
    } finally {
      await fresh.quit();
    }
  });

  it('edits while playing leave the probe clean and the engine in step', async () => {
    await engine.apply([{ op: 'reset' }]);
    const projector = new Projector(engine, resolve);
    let playhead = 0;
    const editor = new RandomEditor(seededRandom(6), {
      sampleHashes: library.hashes,
      spanTicks: 4 * 1920,
      maxTracks: 10,
      playheadTicks: () => Math.round(playhead * 1920),
    });
    let state: SessionState = emptyState;
    for (let i = 0; i < 80; i++) state = editor.applyEdit(state, editor.next(state)).state;
    await projector.update(state);

    await engine.transport({ loop: [0, 4], position: 0, play: true });
    while (!(await engine.stats()).playing) await new Promise((r) => setTimeout(r, 20));
    await engine.arm();

    const until = Date.now() + 4000;
    let edits = 0;
    while (Date.now() < until) {
      state = editor.applyEdit(state, editor.next(state)).state;
      await projector.update(state);
      edits++;
      if (edits % 10 === 0) playhead = (await engine.stats()).audibleSec;
      await new Promise((r) => setTimeout(r, 20));
    }

    const stats = await engine.stats();
    await engine.transport({ play: false });
    console.log(`${edits} edits while playing; slowest block ${stats.maxBlockMs.toFixed(2)} ms, ${stats.lateBlocks} late; ` +
      `probe ${stats.detector.probeClicks} clicks, ${stats.detector.probeGaps} gaps; projection missing ${project(state, resolve).missing.length}`);
    expect(stats.playing).toBe(true);
    expect(stats.unexpectedStops).toBe(0);
    expect(stats.detector.seconds).toBeGreaterThan(3);
    expect(stats.detector.probeClicks).toBe(0);
    expect(stats.detector.probeGaps).toBe(0);
    expect(await projector.verify()).toBeUndefined();
  });
});
