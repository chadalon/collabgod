// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The commands the engine understands (contract/README.md), and a reference
 * implementation of what they do to an engine document.
 *
 * The reference is what the C++ side must match. Both sides check themselves against
 * the same examples in contract/examples, and the diff is tested against the reference.
 */

import type { EngineClip, EngineDoc, EngineTrack } from './document';

export type Command =
  /** Add a track straight after `after`, or first (after the probe) when `after` is null. */
  | { readonly op: 'track.add'; readonly id: string; readonly after: string | null }
  | { readonly op: 'track.move'; readonly id: string; readonly after: string | null }
  /** Removes the track and every clip on it. */
  | { readonly op: 'track.remove'; readonly id: string }
  | {
      readonly op: 'clip.add';
      readonly id: string;
      readonly trackId: string;
      readonly start: number;
      readonly length: number;
      readonly sampleHash: string;
      readonly file: string;
    }
  /** Changes only the fields given. The content moves with the clip; the offset stays. */
  | {
      readonly op: 'clip.set';
      readonly id: string;
      readonly trackId?: string;
      readonly start?: number;
      readonly length?: number;
    }
  | { readonly op: 'clip.remove'; readonly id: string }
  /** Removes every track (and clip) the model made. */
  | { readonly op: 'reset' };

export interface ApplyOutcome {
  readonly doc: EngineDoc;
  /** Index of the command that could not be carried out; the ones before it were. */
  readonly failedAt?: number;
  readonly error?: string;
}

interface MutableTrack {
  id: string;
  clips: Map<string, EngineClip>;
}

const byId = (a: { id: string }, b: { id: string }): number => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0);

/** What the engine does with a batch of commands. Stops at the first that cannot be carried out. */
export function applyCommands(doc: EngineDoc, commands: readonly Command[]): ApplyOutcome {
  const tracks: MutableTrack[] = doc.tracks.map((t) => ({ id: t.id, clips: new Map(t.clips.map((c) => [c.id, c])) }));
  const findTrack = (id: string): MutableTrack | undefined => tracks.find((t) => t.id === id);
  const findClip = (id: string): { track: MutableTrack; clip: EngineClip } | undefined => {
    for (const track of tracks) {
      const clip = track.clips.get(id);
      if (clip) return { track, clip };
    }
    return undefined;
  };
  const insertAfter = (track: MutableTrack, after: string | null): string | undefined => {
    if (after === null) {
      tracks.unshift(track);
      return undefined;
    }
    const index = tracks.findIndex((t) => t.id === after);
    if (index < 0) return `no track ${after} to go after`;
    tracks.splice(index + 1, 0, track);
    return undefined;
  };
  const validSpan = (start: number, length: number): boolean =>
    Number.isFinite(start) && Number.isFinite(length) && start >= 0 && length > 0;

  const run = (command: Command): string | undefined => {
    switch (command.op) {
      case 'track.add': {
        if (findTrack(command.id)) return `track ${command.id} already exists`;
        if (command.after !== null && !findTrack(command.after)) return `no track ${command.after} to go after`;
        return insertAfter({ id: command.id, clips: new Map() }, command.after);
      }
      case 'track.move': {
        const track = findTrack(command.id);
        if (!track) return `no track ${command.id}`;
        if (command.after === command.id) return `track ${command.id} cannot go after itself`;
        if (command.after !== null && !findTrack(command.after)) return `no track ${command.after} to go after`;
        tracks.splice(tracks.indexOf(track), 1);
        return insertAfter(track, command.after);
      }
      case 'track.remove': {
        const track = findTrack(command.id);
        if (!track) return `no track ${command.id}`;
        tracks.splice(tracks.indexOf(track), 1);
        return undefined;
      }
      case 'clip.add': {
        if (!validSpan(command.start, command.length)) return `clip ${command.id}: invalid span`;
        if (findClip(command.id)) return `clip ${command.id} already exists`;
        const track = findTrack(command.trackId);
        if (!track) return `clip ${command.id}: no track ${command.trackId}`;
        const { op: _op, trackId: _trackId, ...clip } = command;
        track.clips.set(command.id, { ...clip, offset: 0 });
        return undefined;
      }
      case 'clip.set': {
        const found = findClip(command.id);
        if (!found) return `no clip ${command.id}`;
        if (command.trackId === undefined && command.start === undefined && command.length === undefined) {
          return `clip.set ${command.id} changes nothing`;
        }
        const destination = command.trackId === undefined ? found.track : findTrack(command.trackId);
        if (!destination) return `clip ${command.id}: no track ${command.trackId}`;
        const start = command.start ?? found.clip.start;
        const length = command.length ?? found.clip.length;
        if (!validSpan(start, length)) return `clip ${command.id}: invalid span`;
        found.track.clips.delete(command.id);
        destination.clips.set(command.id, { ...found.clip, start, length });
        return undefined;
      }
      case 'clip.remove': {
        const found = findClip(command.id);
        if (!found) return `no clip ${command.id}`;
        found.track.clips.delete(command.id);
        return undefined;
      }
      case 'reset':
        tracks.length = 0;
        return undefined;
    }
  };

  const finish = (): EngineDoc => ({
    tracks: tracks.map((t): EngineTrack => ({ id: t.id, clips: [...t.clips.values()].sort(byId) })),
  });

  for (const [index, command] of commands.entries()) {
    const error = run(command);
    if (error !== undefined) return { doc: finish(), failedAt: index, error };
  }
  return { doc: finish() };
}
