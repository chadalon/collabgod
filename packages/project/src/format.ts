// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The project file, version 1, and how a project is written to it.
 *
 * The file is the contract between versions of the app (DAW-6), so its shape is
 * written out here field by field and documented in `docs/project-format.md`.
 * Nothing is copied wholesale from the model: each object is rebuilt from its
 * known fields, in a fixed order, so the same state always gives the same text
 * and nothing else (a local path, say) can ride along into the file.
 */

import type {
  Clip,
  HistoryEntry,
  Member,
  SampleRef,
  SessionSnapshot,
  Timeline,
  Track,
  UndoHistory,
} from '@collabgod/session';

/** The value of every project file's `format` field. */
export const PROJECT_FORMAT = 'collabgod.project';

/** The format version this code writes. Older versions are migrated on load (`migrate.ts`). */
export const PROJECT_VERSION = 1;

/** The file name extension the app gives projects. */
export const PROJECT_EXTENSION = '.collabgod';

/** A project as the app saves and opens it. */
export interface Project {
  /** How many operations of the shared order `session` includes; the next one is `seq + 1`. */
  readonly seq: number;
  /** The confirmed state: which project it is, and every track, clip and member, with who made what (SYN-9). */
  readonly session: SessionSnapshot;
  /**
   * The undo and redo history of the member who saved it, so undo still works
   * after the project is closed and opened again. Absent if none was saved.
   * Another member opening the file starts with an empty history.
   */
  readonly history?: UndoHistory;
}

// ---------------------------------------------------------------------------
// Each object, field by field, in the order the file holds them
// ---------------------------------------------------------------------------

const timelineOf = (timeline: Timeline) => ({
  tempo: timeline.tempo,
  timeSignature: { numerator: timeline.timeSignature.numerator, denominator: timeline.timeSignature.denominator },
  rev: { tempo: timeline.rev.tempo, timeSignature: timeline.rev.timeSignature },
  by: { tempo: timeline.by.tempo, timeSignature: timeline.by.timeSignature },
});

const memberOf = (member: Member) => ({
  id: member.id,
  name: member.name,
  colour: member.colour,
  removed: member.removed,
  verifier: { scheme: member.verifier.scheme, value: member.verifier.value },
  createdBy: member.createdBy,
  rev: { name: member.rev.name, colour: member.rev.colour, removed: member.rev.removed },
  by: { name: member.by.name, colour: member.by.colour, removed: member.by.removed },
});

const trackOf = (track: Track) => ({
  id: track.id,
  name: track.name,
  position: track.position,
  createdBy: track.createdBy,
  rev: { name: track.rev.name, position: track.rev.position },
  by: { name: track.by.name, position: track.by.position },
});

/** A sample by content (ADR-0008): file hash, audio hash when known, and a name. Never a path. */
const sampleOf = (sample: SampleRef) =>
  sample.audioHash === undefined
    ? { fileHash: sample.fileHash, name: sample.name }
    : { fileHash: sample.fileHash, audioHash: sample.audioHash, name: sample.name };

const clipFields = <T>(values: {
  readonly trackId: T;
  readonly start: T;
  readonly length: T;
  readonly offset: T;
  readonly gain: T;
  readonly fadeIn: T;
  readonly fadeOut: T;
}) => ({
  trackId: values.trackId,
  start: values.start,
  length: values.length,
  offset: values.offset,
  gain: values.gain,
  fadeIn: values.fadeIn,
  fadeOut: values.fadeOut,
});

const clipOf = (clip: Clip) => ({
  id: clip.id,
  sample: sampleOf(clip.sample),
  ...clipFields<unknown>(clip),
  createdBy: clip.createdBy,
  rev: clipFields(clip.rev),
  by: clipFields(clip.by),
});

/**
 * An undo entry as the model made it. Its inverse is a change like any other and
 * is checked as one when undo applies it, so a damaged entry is skipped, never
 * applied wrongly.
 */
const historyEntryOf = (entry: HistoryEntry) => ({ opId: entry.opId, inverse: entry.inverse });

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/** A list with one item per line, so a change to one clip is a change to one line. */
function list(items: readonly unknown[], indent: string): string {
  if (items.length === 0) return '[]';
  return `[\n${items.map((item) => `${indent}  ${JSON.stringify(item)}`).join(',\n')}\n${indent}]`;
}

/**
 * The project as the text of a version 1 file: UTF-8 JSON, one object per line,
 * ending in a newline. The same project always gives the same text.
 *
 * Pass a confirmed snapshot (`Replica.confirmedSnapshot`), not the view with
 * unconfirmed edits in it: `seq` says how much of the shared order the file holds.
 *
 * @throws Error if `seq` is not a whole number zero or more.
 */
export function saveProject(project: Project): string {
  if (!Number.isSafeInteger(project.seq) || project.seq < 0) throw new Error(`invalid seq ${String(project.seq)}`);
  const { session, history } = project;
  const lines = [
    `  "format": ${JSON.stringify(PROJECT_FORMAT)}`,
    `  "version": ${PROJECT_VERSION}`,
    `  "projectId": ${JSON.stringify(session.projectId)}`,
    `  "seq": ${project.seq}`,
    `  "timeline": ${JSON.stringify(timelineOf(session.timeline))}`,
    `  "members": ${list(session.members.map(memberOf), '  ')}`,
    `  "tracks": ${list(session.tracks.map(trackOf), '  ')}`,
    `  "clips": ${list(session.clips.map(clipOf), '  ')}`,
  ];
  if (history !== undefined) {
    lines.push(
      [
        '  "history": {',
        `    "member": ${JSON.stringify(history.member)},`,
        `    "undo": ${list(history.undo.map(historyEntryOf), '    ')},`,
        `    "redo": ${list(history.redo.map(historyEntryOf), '    ')}`,
        '  }',
      ].join('\n'),
    );
  }
  return `{\n${lines.join(',\n')}\n}\n`;
}
