// SPDX-License-Identifier: GPL-3.0-or-later

import {
  type ApplyResult,
  canonical,
  type Change,
  fileTimeAt,
  type NewClip,
  type Operation,
  type SampleRef,
  Session,
  type SessionView,
} from '../src';

/** A well-formed SHA-256 text for test number `n`. */
export const hash = (n: number): string => n.toString(16).padStart(64, '0');

export const sample = (n: number, name = `sample-${n}.wav`): SampleRef => ({ fileHash: hash(n), name });

export function newClip(
  id: string,
  trackId: string,
  start: number,
  length: number,
  extra: Partial<NewClip> = {},
): NewClip {
  return { id, trackId, start, length, offset: 0, gain: 0, fadeIn: 0, fadeOut: 0, sample: sample(1), ...extra };
}

let counter = 0;

/** Wrap a change as an operation with a fresh id. */
export function op(change: Change, author = 'm1', id = `test:${(counter += 1)}`): Operation {
  return { id, author, change };
}

/** Apply a change and fail the test if it does not apply. Returns the inverse. */
export function must(session: Session, change: Change, author?: string, id?: string): Change {
  const result = session.apply(op(change, author, id));
  if (!result.applied) throw new Error(`${change.type} did not apply: ${result.reason}`);
  return result.inverse;
}

export function reason(result: ApplyResult): string {
  return result.applied ? 'applied' : result.reason;
}

/** The state that results from applying operations in order to an empty session. */
export function fold(operations: Iterable<Operation>, initial: Session = Session.empty()): Session {
  const session = initial;
  for (const operation of operations) session.apply(operation);
  return session;
}

/** Run `body` and check that the session afterwards is exactly as before. */
export function unchanged(session: SessionView, body: () => void): void {
  const before = canonical(session);
  body();
  const after = canonical(session);
  if (before !== after) throw new Error(`session changed:\n${before}\n${after}`);
}

/**
 * The trim the interface would send for a clip whose edges a person dropped at
 * these ticks, worked out in `view` as that person sees it.
 */
export function trimTo(view: SessionView, clipId: string, edges: { start?: number; end?: number }): Change {
  const clip = view.clips.get(clipId);
  if (clip === undefined) throw new Error(`no clip ${clipId}`);
  const at = (tick: number) => fileTimeAt(clip, tick, view.timeline.tempo);
  if (edges.start !== undefined && edges.end !== undefined) {
    return { type: 'clip.trim', clipId, from: at(edges.start), to: at(edges.end) };
  }
  if (edges.start !== undefined) return { type: 'clip.trim', clipId, from: at(edges.start) };
  if (edges.end !== undefined) return { type: 'clip.trim', clipId, to: at(edges.end) };
  return { type: 'clip.trim', clipId };
}

/** Two tracks and three clips, so the first random edits already have targets. */
export const SEED: readonly Change[] = [
  { type: 'track.add', track: { id: 't1', name: 'Drums', position: 'i' } },
  { type: 'track.add', track: { id: 't2', name: 'Bass', position: 'r' } },
  { type: 'clip.add', clip: newClip('k1', 't1', 0, 960, { sample: sample(1, 'kick.wav') }) },
  { type: 'clip.add', clip: newClip('k2', 't1', 1920, 960, { sample: sample(2, 'snare.wav') }) },
  {
    type: 'clip.add',
    clip: newClip('k3', 't2', 0, 3840, { sample: { fileHash: hash(3), audioHash: hash(33), name: 'sub.aif' } }),
  },
];

const scale = Number(
  (globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env['FC_SCALE'] ?? 1,
);

/** Number of random cases to run. Set FC_SCALE=40 for a longer soak. */
export const runs = (n: number): number => Math.max(1, Math.round(n * scale));
