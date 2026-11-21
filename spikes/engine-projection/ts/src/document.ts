// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The engine document: what the engine should hold, in the engine's own terms.
 *
 * `project` is a pure function from the session model to this document. It is the
 * only place that knows how the model maps onto the engine: ticks become seconds,
 * sample hashes become files on this machine, and clips whose sample is not on this
 * machine are left out. Everything after it (the diff, the commands, the readback)
 * works on documents and knows nothing about the model.
 *
 * The same shape is what the engine reports back (`readback`), so "has the engine
 * drifted from the model?" is a comparison of two documents.
 */

import type { SessionState } from '../../../session-model/src/model';

export interface EngineClip {
  readonly id: string;
  /** Seconds on the edit's timeline. */
  readonly start: number;
  readonly length: number;
  /** Seconds into the file where the clip's content starts. The model has no offset yet, so always 0. */
  readonly offset: number;
  readonly sampleHash: string;
  /**
   * The file on this machine. A local path goes from the TypeScript side to the engine
   * on the same machine only; it never goes into the session or over the network.
   */
  readonly file: string;
}

export interface EngineTrack {
  readonly id: string;
  /** Sorted by id, as the engine reports them. */
  readonly clips: readonly EngineClip[];
}

export interface EngineDoc {
  /** In the order they appear in the engine, after the probe track. */
  readonly tracks: readonly EngineTrack[];
}

export const emptyDoc: EngineDoc = Object.freeze({ tracks: Object.freeze([]) });

/** How model ticks map to seconds. A fixed tempo for now: the model has no tempo yet. */
export interface Timebase {
  readonly ticksPerSecond: number;
}

/** 960 ticks per beat at 120 beats per minute. */
export const defaultTimebase: Timebase = { ticksPerSecond: 1920 };

/** Finds the file for a sample on this machine, or undefined if it is not here. */
export type SampleResolver = (sampleHash: string) => string | undefined;

export interface Projection {
  readonly doc: EngineDoc;
  /** Clips left out because their sample is not on this machine. */
  readonly missing: readonly string[];
}

const byId = (a: { id: string }, b: { id: string }): number => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0);

/** The model's state as the engine should hold it. Pure and deterministic. */
export function project(state: SessionState, resolve: SampleResolver, timebase: Timebase = defaultTimebase): Projection {
  const clipsByTrack = new Map<string, EngineClip[]>();
  const missing: string[] = [];

  for (const clip of Object.values(state.clips)) {
    const file = resolve(clip.sampleHash);
    if (file === undefined) {
      missing.push(clip.id);
      continue;
    }
    const list = clipsByTrack.get(clip.trackId) ?? [];
    list.push({
      id: clip.id,
      start: clip.start / timebase.ticksPerSecond,
      length: clip.length / timebase.ticksPerSecond,
      offset: 0,
      sampleHash: clip.sampleHash,
      file,
    });
    clipsByTrack.set(clip.trackId, list);
  }

  return {
    doc: {
      tracks: state.trackOrder.map((id) => ({ id, clips: (clipsByTrack.get(id) ?? []).sort(byId) })),
    },
    missing: missing.sort(),
  };
}

/** Times within a microsecond are the same time: the engine stores doubles. */
export const sameTime = (a: number, b: number): boolean => Math.abs(a - b) <= 1e-6;

export const sameClip = (a: EngineClip, b: EngineClip): boolean =>
  a.id === b.id &&
  sameTime(a.start, b.start) &&
  sameTime(a.length, b.length) &&
  sameTime(a.offset, b.offset) &&
  a.sampleHash === b.sampleHash &&
  a.file === b.file;

/** The first difference between two documents, or undefined if they are the same. */
export function firstDifference(actual: EngineDoc, expected: EngineDoc): string | undefined {
  const actualIds = actual.tracks.map((t) => t.id).join(',');
  const expectedIds = expected.tracks.map((t) => t.id).join(',');
  if (actualIds !== expectedIds) return `tracks [${actualIds}], expected [${expectedIds}]`;

  for (const [i, track] of actual.tracks.entries()) {
    const other = expected.tracks[i];
    if (other === undefined) return `track ${track.id} is extra`;
    if (track.clips.length !== other.clips.length) {
      return `track ${track.id} has clips [${track.clips.map((c) => c.id).join(',')}], expected [${other.clips.map((c) => c.id).join(',')}]`;
    }
    for (const [j, clip] of track.clips.entries()) {
      const want = other.clips[j];
      if (want === undefined || !sameClip(clip, want)) {
        return `track ${track.id}, clip ${clip.id}: ${JSON.stringify(clip)}, expected ${JSON.stringify(want)}`;
      }
    }
  }
  return undefined;
}
