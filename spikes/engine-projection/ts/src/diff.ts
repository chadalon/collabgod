// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The smallest list of commands that turns one engine document into another.
 *
 * Used two ways:
 * - after an edit: from the document last sent to the engine, to the new projection;
 * - to rebuild: from what the engine reports it holds (readback), or from an empty
 *   engine, to the projection. That is how the engine can be rebuilt from the model
 *   at any time.
 *
 * Order matters: a clip can only be added to a track that exists, and an id can only
 * be reused once the old item is gone. The commands go:
 *   1. remove clips whose sample or file changed (they come back in step 3)
 *   2. add and move tracks into the target order
 *   3. add clips
 *   4. change clips that moved or changed length
 *   5. remove clips that are gone (except those on tracks about to be removed)
 *   6. remove tracks that are gone, with whatever is still on them
 */

import type { Command } from './commands';
import { type EngineClip, type EngineDoc, sameTime } from './document';

interface Placed {
  readonly trackId: string;
  readonly clip: EngineClip;
}

function index(doc: EngineDoc): Map<string, Placed> {
  const clips = new Map<string, Placed>();
  for (const track of doc.tracks) for (const clip of track.clips) clips.set(clip.id, { trackId: track.id, clip });
  return clips;
}

const sameSource = (a: EngineClip, b: EngineClip): boolean =>
  a.sampleHash === b.sampleHash && a.file === b.file && sameTime(a.offset, b.offset);

export function diff(from: EngineDoc, to: EngineDoc): Command[] {
  const commands: Command[] = [];
  const fromClips = index(from);
  const toClips = index(to);
  const toTrackIds = new Set(to.tracks.map((t) => t.id));
  const removedTracks = new Set(from.tracks.map((t) => t.id).filter((id) => !toTrackIds.has(id)));

  // 1. A clip whose source changed is removed and added again.
  const readd = new Set<string>();
  for (const [id, before] of fromClips) {
    const after = toClips.get(id);
    if (after && !sameSource(before.clip, after.clip)) {
      readd.add(id);
      commands.push({ op: 'clip.remove', id });
    }
  }

  // 2. Tracks into the target order. Tracks about to be removed are ignored: only the
  //    order of the tracks that stay matters, and the removed ones go in step 6.
  const order = from.tracks.map((t) => t.id).filter((id) => !removedTracks.has(id));
  for (const [i, id] of to.tracks.map((t) => t.id).entries()) {
    if (order[i] === id) continue;
    const after = i === 0 ? null : (to.tracks[i - 1]?.id ?? null);
    const existing = order.indexOf(id);
    if (existing >= 0) {
      order.splice(existing, 1);
      commands.push({ op: 'track.move', id, after });
    } else {
      commands.push({ op: 'track.add', id, after });
    }
    order.splice(i, 0, id);
  }

  // 3. New clips, and the ones removed in step 1.
  for (const [id, { trackId, clip }] of toClips) {
    if (fromClips.has(id) && !readd.has(id)) continue;
    commands.push({
      op: 'clip.add',
      id,
      trackId,
      start: clip.start,
      length: clip.length,
      sampleHash: clip.sampleHash,
      file: clip.file,
    });
  }

  // 4. Clips that moved or changed length: only the fields that changed.
  for (const [id, after] of toClips) {
    const before = fromClips.get(id);
    if (!before || readd.has(id)) continue;
    const set: { -readonly [K in 'trackId' | 'start' | 'length']?: K extends 'trackId' ? string : number } = {};
    if (before.trackId !== after.trackId) set.trackId = after.trackId;
    if (!sameTime(before.clip.start, after.clip.start)) set.start = after.clip.start;
    if (!sameTime(before.clip.length, after.clip.length)) set.length = after.clip.length;
    if (Object.keys(set).length > 0) commands.push({ op: 'clip.set', id, ...set });
  }

  // 5. Clips that are gone, unless their track is going too.
  for (const [id, before] of fromClips) {
    if (!toClips.has(id) && !removedTracks.has(before.trackId)) commands.push({ op: 'clip.remove', id });
  }

  // 6. Tracks that are gone.
  for (const id of removedTracks) commands.push({ op: 'track.remove', id });

  return commands;
}
