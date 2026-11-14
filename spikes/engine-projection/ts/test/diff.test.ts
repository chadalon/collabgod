// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { applyCommands } from '../src/commands';
import { diff } from '../src/diff';
import { type EngineClip, type EngineDoc, emptyDoc, firstDifference } from '../src/document';

const clip = (id: string, start = 0, length = 1, sampleHash = 'h'): EngineClip => ({ id, start, length, offset: 0, sampleHash, file: `/s/${sampleHash}.wav` });

describe('diff', () => {
  const base: EngineDoc = {
    tracks: [
      { id: 't1', clips: [clip('a', 0), clip('b', 2)] },
      { id: 't2', clips: [clip('c', 1)] },
    ],
  };

  it('sends nothing when nothing changed', () => {
    expect(diff(base, base)).toEqual([]);
  });

  it('a clip move is one command with only what changed', () => {
    const to: EngineDoc = { tracks: [{ id: 't1', clips: [clip('a', 0), clip('b', 3)] }, base.tracks[1] as never] };
    expect(diff(base, to)).toEqual([{ op: 'clip.set', id: 'b', start: 3 }]);
  });

  it('a move to another track carries the track', () => {
    const to: EngineDoc = { tracks: [{ id: 't1', clips: [clip('a', 0)] }, { id: 't2', clips: [clip('b', 2), clip('c', 1)] }] };
    expect(diff(base, to)).toEqual([{ op: 'clip.set', id: 'b', trackId: 't2' }]);
  });

  it('removing a track does not remove its clips one by one', () => {
    const to: EngineDoc = { tracks: [base.tracks[1] as never] };
    expect(diff(base, to)).toEqual([{ op: 'track.remove', id: 't1' }]);
  });

  it('a clip whose sample changed is removed and added again', () => {
    const to: EngineDoc = { tracks: [{ id: 't1', clips: [clip('a', 0, 1, 'other'), clip('b', 2)] }, base.tracks[1] as never] };
    expect(diff(base, to).map((c) => c.op)).toEqual(['clip.remove', 'clip.add']);
  });

  it('builds a whole document from empty', () => {
    expect(applyCommands(emptyDoc, diff(emptyDoc, base)).doc).toEqual(base);
  });

  // Documents built from a small pool of ids, so the two sides share tracks and clips.
  const docArb: fc.Arbitrary<EngineDoc> = fc
    .record({
      trackIds: fc.uniqueArray(fc.constantFrom('t1', 't2', 't3', 't4', 't5'), { maxLength: 5 }),
      clips: fc.array(
        fc.record({
          id: fc.constantFrom('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'),
          track: fc.nat(4),
          start: fc.integer({ min: 0, max: 40 }).map((n) => n / 4),
          length: fc.integer({ min: 1, max: 16 }).map((n) => n / 4),
          sample: fc.constantFrom('h1', 'h2'),
        }),
        { maxLength: 10 },
      ),
    })
    .map(({ trackIds, clips }) => {
      const seen = new Set<string>();
      return {
        tracks: trackIds.map((id, i) => ({
          id,
          clips: clips
            .filter((c) => c.track % Math.max(1, trackIds.length) === i && !seen.has(c.id) && (seen.add(c.id), true))
            .map((c) => clip(c.id, c.start, c.length, c.sample))
            .sort((x, y) => (x.id < y.id ? -1 : 1)),
        })),
      };
    });

  it('property: applying the diff of any two documents to the first gives the second', () => {
    fc.assert(
      fc.property(docArb, docArb, (from, to) => {
        const outcome = applyCommands(from, diff(from, to));
        expect(outcome.failedAt).toBeUndefined();
        expect(firstDifference(outcome.doc, to)).toBeUndefined();
      }),
      { numRuns: 2000 },
    );
  });

  it('property: the diff of a document with itself is empty', () => {
    fc.assert(fc.property(docArb, (doc) => diff(doc, doc).length === 0));
  });
});
