// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { apply, emptyState, type Op, type SessionState } from '../../../session-model/src/model';
import { firstDifference, project } from '../src/document';

const build = (ops: Op[]): SessionState =>
  ops.reduce((state, op, i) => {
    const result = apply(state, op, `op${i}`);
    if (!result.applied) throw new Error(`setup op ${i} rejected: ${result.reason}`);
    return result.state;
  }, emptyState);

const files: Record<string, string> = { h1: '/samples/one.wav', h2: '/samples/two.wav' };
const resolve = (hash: string) => files[hash];

describe('project', () => {
  const state = build([
    { type: 'track.add', track: { id: 'b', name: 'B' }, index: 0 },
    { type: 'track.add', track: { id: 'a', name: 'A' }, index: 1 },
    { type: 'clip.add', clip: { id: 'z', trackId: 'a', start: 1920, length: 960, sampleHash: 'h1' } },
    { type: 'clip.add', clip: { id: 'y', trackId: 'a', start: 0, length: 3840, sampleHash: 'h2' } },
    { type: 'clip.add', clip: { id: 'x', trackId: 'b', start: 480, length: 480, sampleHash: 'not-here' } },
  ]);

  it('keeps the model track order and sorts clips by id', () => {
    const { doc } = project(state, resolve);
    expect(doc.tracks.map((t) => t.id)).toEqual(['b', 'a']);
    expect(doc.tracks[1]?.clips.map((c) => c.id)).toEqual(['y', 'z']);
  });

  it('turns ticks into seconds at 1920 ticks a second', () => {
    const z = project(state, resolve).doc.tracks[1]?.clips[1];
    expect(z).toMatchObject({ start: 1, length: 0.5, offset: 0, sampleHash: 'h1', file: '/samples/one.wav' });
  });

  it('leaves out clips whose sample is not on this machine, and says which', () => {
    const { doc, missing } = project(state, resolve);
    expect(doc.tracks[0]?.clips).toEqual([]);
    expect(missing).toEqual(['x']);
  });

  it('is a pure function of the state', () => {
    expect(project(state, resolve)).toEqual(project(state, resolve));
  });

  it('a different timebase gives different seconds', () => {
    const z = project(state, resolve, { ticksPerSecond: 960 }).doc.tracks[1]?.clips[1];
    expect(z?.start).toBe(2);
  });
});

describe('firstDifference', () => {
  const clip = { id: 'c', start: 1, length: 1, offset: 0, sampleHash: 'h', file: '/f' };
  it('ignores sub-microsecond differences', () => {
    expect(firstDifference({ tracks: [{ id: 't', clips: [clip] }] }, { tracks: [{ id: 't', clips: [{ ...clip, start: 1 + 1e-9 }] }] })).toBeUndefined();
  });
  it('names what differs', () => {
    expect(firstDifference({ tracks: [{ id: 't', clips: [clip] }] }, { tracks: [{ id: 't', clips: [{ ...clip, start: 2 }] }] })).toMatch(/clip c/);
    expect(firstDifference({ tracks: [] }, { tracks: [{ id: 't', clips: [] }] })).toMatch(/tracks/);
  });
});
