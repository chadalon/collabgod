// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { type Change, opIdSource, Replica, Session } from '../src';
import { newClip, sample } from './helpers';

declare const performance: { now(): number };
declare const console: { log(message: string): void };

/**
 * Issue #23's budget: receiving one remote edit on a 5,000-clip session with 20
 * unconfirmed local edits takes under 4 ms, a quarter of a frame (accepted by
 * Chad, 2026-11-22). The spike's whole-state copy took 41 ms for this.
 */
const BUDGET_MS = 4;

/** A busy real project: 50 tracks, 5,000 clips. */
function bigSession(tracks: number, clips: number): Session {
  const session = Session.empty();
  let n = 0;
  const apply = (change: Change) => session.apply({ id: `seed:${(n += 1)}`, author: 'seed', change });
  for (let t = 0; t < tracks; t += 1) {
    apply({ type: 'track.add', track: { id: `t${t}`, name: `Track ${t}`, position: `${(t + 10).toString(36)}1` } });
  }
  for (let c = 0; c < clips; c += 1) {
    apply({ type: 'clip.add', clip: newClip(`c${c}`, `t${c % tracks}`, c * 960, 960, { sample: sample(c % 200) }) });
  }
  return session;
}

/** Twenty unconfirmed edits of different kinds, the sort a person makes in a burst. */
function localEdits(me: Replica, clips: number): void {
  for (let i = 0; i < 20; i += 1) {
    const clipId = `c${(i * 97) % clips}`;
    const changes: Change[] = [
      { type: 'clip.move', clipId, trackId: `t${i % 7}`, start: 1_000_000 + i * 960 },
      { type: 'clip.trim', clipId, start: ((i * 97) % clips) * 960 + 120 },
      { type: 'clip.gain', clipId, gain: -3 },
      { type: 'clip.split', clipId, newClipId: `mine-${i}`, at: 480 },
      { type: 'track.rename', trackId: `t${i % 50}`, name: `Mine ${i}` },
    ];
    const change = changes[i % changes.length];
    if (change === undefined || !me.submit(change).applied) throw new Error(`local edit ${i} did not apply`);
  }
}

/** Someone else's edit, ordered by the relay. */
const remote = (i: number, clips: number): Change =>
  i % 3 === 0
    ? { type: 'clip.move', clipId: `c${(i * 31 + 7) % clips}`, trackId: `t${i % 50}`, start: 2_000_000 + i }
    : i % 3 === 1
      ? { type: 'clip.fade', clipId: `c${(i * 17 + 3) % clips}`, fadeIn: i % 480, fadeOut: 120 }
      : { type: 'track.rename', trackId: `t${(i * 13) % 50}`, name: `Theirs ${i}` };

function measure(clips: number): { median: number; p95: number; max: number } {
  const me = new Replica({ author: 'me', nextOpId: opIdSource('me:'), session: bigSession(50, clips) });
  localEdits(me, clips);
  const samples: number[] = [];
  for (let i = 0; i < 700; i += 1) {
    const started = performance.now();
    me.receive({ seq: me.seq + 1, operation: { id: `them:${i}`, author: 'them', change: remote(i, clips) } });
    samples.push(performance.now() - started);
  }
  if (me.unconfirmed.length !== 20) throw new Error('the local edits should still be unconfirmed');
  const sorted = samples.slice(200).sort((a, b) => a - b); // the first 200 warm up the JIT
  const at = (q: number) => sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))] ?? Number.NaN;
  return { median: at(0.5), p95: at(0.95), max: at(1) };
}

const ms = (value: number) => `${value.toFixed(3)} ms`;

describe('timing', () => {
  it(`receives one remote edit on a 5,000-clip session with 20 unconfirmed local edits in under ${BUDGET_MS} ms`, () => {
    const small = measure(500);
    const big = measure(5000);
    console.log(
      `receive one remote edit, 20 unconfirmed: 500 clips median ${ms(small.median)}, p95 ${ms(small.p95)}; ` +
        `5,000 clips median ${ms(big.median)}, p95 ${ms(big.p95)}, max ${ms(big.max)}`,
    );
    expect(big.p95).toBeLessThan(BUDGET_MS);
  });

  it('removes and restores a track holding 100 clips quickly too', () => {
    const session = bigSession(50, 5000);
    const samples: number[] = [];
    for (let i = 0; i < 200; i += 1) {
      const started = performance.now();
      const removed = session.apply({
        id: `x:${i}`,
        author: 'me',
        change: { type: 'track.remove', trackId: `t${i % 50}` },
      });
      if (!removed.applied || !session.apply({ id: `y:${i}`, author: 'me', change: removed.inverse }).applied) {
        throw new Error('remove and restore should apply');
      }
      samples.push(performance.now() - started);
    }
    const median = samples.slice(50).sort((a, b) => a - b)[75] ?? Number.NaN;
    console.log(`remove and restore a track holding 100 clips: median ${ms(median)}`);
    expect(median).toBeLessThan(BUDGET_MS);
  });
});
