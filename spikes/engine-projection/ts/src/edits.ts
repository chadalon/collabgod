// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Random edits to the session model, for the property tests and the dropout check.
 * Every edit goes through the model's own `apply`, including undo by the inverse
 * operation it returns, so the projection sees exactly what the app would.
 */

import { apply, type Op, type SessionState } from '../../../session-model/src/model';

/** A small seeded random number generator (mulberry32), so runs can be repeated. */
export function seededRandom(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export interface EditorOptions {
  readonly sampleHashes: readonly string[];
  /** Clips are placed within this many ticks. */
  readonly spanTicks: number;
  readonly maxTracks: number;
  /** Optional: where the playhead is, in ticks, so some edits land right on it. */
  readonly playheadTicks?: () => number;
}

export interface Edit {
  readonly op: Op;
  readonly description: string;
}

export class RandomEditor {
  private counter = 0;
  private readonly undo: Op[] = [];

  constructor(
    private readonly random: () => number,
    private readonly options: EditorOptions,
  ) {}

  private int(n: number): number {
    return Math.floor(this.random() * n);
  }

  private pick<T>(items: readonly T[]): T | undefined {
    return items.length === 0 ? undefined : items[this.int(items.length)];
  }

  private start(): number {
    const near = this.options.playheadTicks?.();
    if (near !== undefined && this.random() < 0.5) {
      const window = Math.round(this.options.spanTicks / 20);
      return Math.max(0, Math.round(near - window + this.random() * 2 * window));
    }
    return this.int(this.options.spanTicks);
  }

  private length(): number {
    return 1 + this.int(Math.round(this.options.spanTicks / 4));
  }

  /** Picks an edit that makes sense for this state. */
  next(state: SessionState): Edit {
    const tracks = state.trackOrder;
    const clips = Object.values(state.clips);
    const roll = this.random();
    const id = () => `${++this.counter}`;

    if (tracks.length === 0 || (roll < 0.05 && tracks.length < this.options.maxTracks)) {
      const trackId = `t${id()}`;
      return { op: { type: 'track.add', track: { id: trackId, name: trackId }, index: this.int(tracks.length + 1) }, description: `add track ${trackId}` };
    }
    if (roll < 0.08 && this.undo.length > 0) {
      const op = this.undo.pop() as Op;
      return { op, description: `undo (${op.type})` };
    }
    if (roll < 0.1 && tracks.length > 1) {
      const trackId = this.pick(tracks) as string;
      return { op: { type: 'track.remove', id: trackId }, description: `remove track ${trackId}` };
    }
    if (clips.length === 0 || roll < 0.3) {
      const clipId = `c${id()}`;
      const trackId = this.pick(tracks) as string;
      const sampleHash = this.pick(this.options.sampleHashes) as string;
      return {
        op: { type: 'clip.add', clip: { id: clipId, trackId, start: this.start(), length: this.length(), sampleHash } },
        description: `add clip ${clipId}`,
      };
    }
    const clip = this.pick(clips);
    if (clip === undefined) throw new Error('unreachable');
    if (roll < 0.4) return { op: { type: 'clip.remove', id: clip.id }, description: `remove clip ${clip.id}` };
    if (roll < 0.75) {
      const trackId = this.random() < 0.3 ? (this.pick(tracks) as string) : clip.trackId;
      return { op: { type: 'clip.move', id: clip.id, trackId, start: this.start() }, description: `move clip ${clip.id}` };
    }
    return {
      op: { type: 'clip.resize', id: clip.id, start: clip.start, length: this.length() },
      description: `resize clip ${clip.id}`,
    };
  }

  /** Applies an edit through the model. Returns the new state; a rejected edit leaves it as it was. */
  applyEdit(state: SessionState, edit: Edit): { state: SessionState; applied: boolean } {
    const result = apply(state, edit.op, `op-${++this.counter}`);
    if (!result.applied) return { state, applied: false };
    this.undo.push(result.inverse);
    if (this.undo.length > 50) this.undo.shift();
    return { state: result.state, applied: true };
  }
}
