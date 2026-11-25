// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { CommandQueue, type FrameScheduler } from '../src/commands';
import type { Command } from '../src/contract';

/** Animation frames and timers that run only when the test says so. */
function fakeScheduler() {
  let next = 1;
  const frames = new Map<number, () => void>();
  const timers = new Map<number, { fn: () => void; ms: number }>();
  const scheduler: FrameScheduler = {
    requestFrame: (fn) => (frames.set(next, fn), next++),
    cancelFrame: (h) => frames.delete(h as number),
    setTimeout: (fn, ms) => (timers.set(next, { fn, ms }), next++),
    clearTimeout: (h) => timers.delete(h as number),
  };
  return {
    scheduler,
    frames,
    timers,
    /** Runs the animation frame callbacks waiting now, as the browser does at a display refresh. */
    frame: () => [...frames.entries()].forEach(([h, fn]) => (frames.delete(h), fn())),
    fireTimers: () => [...timers.entries()].forEach(([h, t]) => (timers.delete(h), t.fn())),
  };
}

function queue() {
  const sent: Command[][] = [];
  const clock = fakeScheduler();
  const q = new CommandQueue(async (batch) => void sent.push([...batch]), { scheduler: clock.scheduler, fallbackMs: 100 });
  return { q, sent, clock };
}

describe('CommandQueue', () => {
  it('sends everything queued in a frame as one batch, on the next animation frame', () => {
    const { q, sent, clock } = queue();
    q.push('probe', { id: 1 });
    q.push('probe', { id: 2 });
    q.push('probe', { id: 3 });
    expect(sent).toHaveLength(0);
    expect(clock.frames.size).toBe(1);
    clock.frame();
    expect(sent).toEqual([[1, 2, 3].map((id) => ({ type: 'probe', args: { id } }))]);
    expect(q.batches).toBe(1);
    expect(q.sent).toBe(3);
    expect(q.length).toBe(0);
  });

  it('sends at most one batch per display frame', () => {
    const { q, sent, clock } = queue();
    for (let frame = 0; frame < 5; frame++) {
      for (let i = 0; i < 4; i++) q.push('probe', { id: frame * 10 + i });
      clock.frame();
      clock.frame(); // a second refresh with nothing new sends nothing
    }
    expect(sent).toHaveLength(5);
    expect(sent.every((batch) => batch.length === 4)).toBe(true);
  });

  it('keeps the order commands were queued in', () => {
    const { q, sent, clock } = queue();
    [5, 3, 9].forEach((id) => q.push('probe', { id }));
    clock.frame();
    expect(sent[0]!.map((c) => c.args.id)).toEqual([5, 3, 9]);
  });

  it('sends after the fallback when the page draws no frames, and once only', () => {
    const { q, sent, clock } = queue();
    q.push('probe', { id: 1 });
    expect([...clock.timers.values()].map((t) => t.ms)).toEqual([100]);
    clock.fireTimers();
    expect(sent).toHaveLength(1);
    expect(clock.frames.size).toBe(0); // the frame that was waiting is cancelled
    clock.frame();
    expect(sent).toHaveLength(1);
  });

  it('checks arguments when a command is queued', () => {
    const { q, clock } = queue();
    expect(() => q.push('probe', { id: 1.5 })).toThrow('probe.args.id is 1.5, not a whole number');
    expect(q.length).toBe(0);
    expect(clock.frames.size).toBe(0);
  });

  it('reports a batch the app refused, with the batch', async () => {
    const clock = fakeScheduler();
    const errors: [unknown, readonly Command[]][] = [];
    const q = new CommandQueue(() => Promise.reject(new Error('refused')), {
      scheduler: clock.scheduler,
      onError: (e, b) => errors.push([e, b]),
    });
    q.push('probe', { id: 7 });
    clock.frame();
    await Promise.resolve();
    expect(errors).toHaveLength(1);
    expect(errors[0]![1]).toEqual([{ type: 'probe', args: { id: 7 } }]);
  });
});
