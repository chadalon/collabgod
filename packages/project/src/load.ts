// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Reading a project file: recognise it, migrate an older version to the current
 * one, then check every field before anything reaches the model.
 *
 * A file may come from another person or another version of the app, so nothing
 * in it is trusted. A file that cannot be read gives a reason the interface can
 * show, never an exception. A sample that is not on this machine is not an
 * error at all: loading never looks for samples (see `locations.ts`).
 */

import { type HistoryEntry, Session, type SessionSnapshot, type UndoHistory } from '@collabgod/session';
import { PROJECT_FORMAT, PROJECT_VERSION, type Project } from './format';
import { MIGRATIONS, type Migration } from './migrate';

export type LoadErrorKind =
  /** Not JSON, or JSON that is not a CollabGod project. */
  | 'not-a-project'
  /** Saved by a newer version of the app, in a format this one does not know. */
  | 'newer-version'
  /** An old version this app can no longer migrate. */
  | 'unsupported-version'
  /** A project file, but a field is missing, malformed or breaks a rule of the session. */
  | 'damaged';

export interface LoadError {
  readonly kind: LoadErrorKind;
  /** What is wrong, for the log and for a bug report. Not worded for the person. */
  readonly detail: string;
  /** The file's format version, when it has a readable one. */
  readonly version?: number;
}

export type LoadResult =
  | {
      readonly ok: true;
      readonly project: Project;
      /**
       * The version the file was in, when it was older and has been migrated.
       * The app should keep the original file before saving over it, since an
       * older app cannot open the new one.
       */
      readonly migratedFrom?: number;
    }
  | { readonly ok: false; readonly error: LoadError };

export interface LoadOptions {
  /** The migrations to use. Tests pass their own; the app uses the default. */
  readonly migrations?: readonly Migration[];
}

type Plain = Readonly<Record<string, unknown>>;

const isObject = (value: unknown): value is Plain =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

class Damaged extends Error {}

const damaged = (detail: string): never => {
  throw new Damaged(detail);
};

/** `value` is an object with exactly the keys in `required`, plus any of `optional`. */
function keys(value: unknown, where: string, required: readonly string[], optional: readonly string[] = []): Plain {
  if (!isObject(value)) return damaged(`${where} is not an object`);
  for (const key of required) if (!Object.hasOwn(value, key)) damaged(`${where} has no ${key}`);
  for (const key of Object.keys(value)) {
    if (!required.includes(key) && !optional.includes(key)) damaged(`${where} has an unknown field ${key}`);
  }
  return value;
}

function list(value: unknown, where: string): readonly unknown[] {
  return Array.isArray(value) ? value : damaged(`${where} is not a list`);
}

const TIMELINE_FIELDS = ['tempo', 'timeSignature'];
const MEMBER_FIELDS = ['name', 'colour', 'removed'];
const TRACK_FIELDS = ['name', 'position'];
const CLIP_FIELDS = ['trackId', 'start', 'length', 'offset', 'gain', 'fadeIn', 'fadeOut'];

/** Revisions and authors name every editable field and nothing else. Their values are the model's to check. */
function stamps(value: Plain, where: string, fields: readonly string[]): void {
  keys(value['rev'], `${where} rev`, fields);
  keys(value['by'], `${where} by`, fields);
}

/**
 * The session part of a version 1 file. Shapes are checked here, key by key, so
 * a field the format does not define is refused; values and the rules between
 * objects are checked by the model, exactly as for an operation off the network.
 */
function sessionOf(file: Plain): SessionSnapshot {
  const timeline = keys(file['timeline'], 'timeline', [...TIMELINE_FIELDS, 'rev', 'by']);
  keys(timeline['timeSignature'], 'timeline timeSignature', ['numerator', 'denominator']);
  stamps(timeline, 'timeline', TIMELINE_FIELDS);

  list(file['members'], 'members').forEach((value, i) => {
    const member = keys(value, `member ${i}`, ['id', ...MEMBER_FIELDS, 'verifier', 'createdBy', 'rev', 'by']);
    keys(member['verifier'], `member ${i} verifier`, ['scheme', 'value']);
    stamps(member, `member ${i}`, MEMBER_FIELDS);
  });
  list(file['tracks'], 'tracks').forEach((value, i) => {
    stamps(keys(value, `track ${i}`, ['id', ...TRACK_FIELDS, 'createdBy', 'rev', 'by']), `track ${i}`, TRACK_FIELDS);
  });
  list(file['clips'], 'clips').forEach((value, i) => {
    const clip = keys(value, `clip ${i}`, ['id', 'sample', ...CLIP_FIELDS, 'createdBy', 'rev', 'by']);
    keys(clip['sample'], `clip ${i} sample`, ['fileHash', 'name'], ['audioHash']);
    stamps(clip, `clip ${i}`, CLIP_FIELDS);
  });

  const snapshot = {
    projectId: file['projectId'],
    timeline: file['timeline'],
    members: file['members'],
    tracks: file['tracks'],
    clips: file['clips'],
  } as unknown as SessionSnapshot;
  try {
    // The model checks every value and every rule between objects, and copies only known fields.
    return Session.fromSnapshot(snapshot).snapshot();
  } catch (error) {
    return damaged(error instanceof Error ? error.message : String(error));
  }
}

function historyOf(value: unknown): UndoHistory {
  const history = keys(value, 'history', ['member', 'undo', 'redo']);
  const member = history['member'];
  if (typeof member !== 'string' || member.length === 0) damaged('history has no member');
  const entries = (side: 'undo' | 'redo'): readonly HistoryEntry[] =>
    list(history[side], `history ${side}`).map((item, i) => {
      const entry = keys(item, `history ${side} ${i}`, ['opId', 'inverse']);
      const { opId, inverse } = entry;
      // The inverse is checked like any operation when undo applies it, and skipped if it does not apply.
      if (typeof opId !== 'string' || opId.length === 0) damaged(`history ${side} ${i} has no opId`);
      if (!isObject(inverse) || typeof inverse['type'] !== 'string') damaged(`history ${side} ${i} has no inverse`);
      return Object.freeze({ opId, inverse }) as HistoryEntry;
    });
  return Object.freeze({
    member: member as string,
    undo: Object.freeze(entries('undo')),
    redo: Object.freeze(entries('redo')),
  });
}

/** Check a document of the current version and turn it into a project. */
function projectOf(file: Plain): Project {
  const fields = ['format', 'version', 'projectId', 'seq', 'timeline', 'members', 'tracks', 'clips'];
  keys(file, 'the file', fields, ['history']);
  const seq = file['seq'];
  if (typeof seq !== 'number' || !Number.isSafeInteger(seq) || seq < 0) damaged('seq is not a whole number');
  const session = sessionOf(file);
  return file['history'] === undefined
    ? { seq: seq as number, session }
    : { seq: seq as number, session, history: historyOf(file['history']) };
}

/**
 * Bring a parsed document from `version` up to the current one, one version at
 * a time. Returns the reason it cannot, if a step is missing.
 */
function migrated(
  file: Plain,
  version: number,
  migrations: readonly Migration[],
): { readonly file: Plain } | { readonly error: LoadError } {
  let current = file;
  for (let from = version; from < PROJECT_VERSION; from += 1) {
    const step = migrations.find((migration) => migration.from === from);
    if (step === undefined) {
      return {
        error: { kind: 'unsupported-version', detail: `no migration from format version ${from}`, version },
      };
    }
    current = step.migrate(current);
    if (current['version'] !== from + 1) damaged(`the migration from version ${from} did not give version ${from + 1}`);
  }
  return { file: current };
}

/** Read a project file's text. Never throws. */
export function loadProject(text: string, options: LoadOptions = {}): LoadResult {
  let data: unknown;
  try {
    data = JSON.parse(text.startsWith('﻿') ? text.slice(1) : text);
  } catch (error) {
    return { ok: false, error: { kind: 'not-a-project', detail: `not JSON: ${(error as Error).message}` } };
  }
  if (!isObject(data) || data['format'] !== PROJECT_FORMAT) {
    return { ok: false, error: { kind: 'not-a-project', detail: `no "format": "${PROJECT_FORMAT}"` } };
  }
  const version = data['version'];
  if (typeof version !== 'number' || !Number.isSafeInteger(version)) {
    return { ok: false, error: { kind: 'damaged', detail: 'version is not a whole number' } };
  }
  if (version > PROJECT_VERSION) {
    return {
      ok: false,
      error: {
        kind: 'newer-version',
        detail: `format version ${version}; this app reads up to ${PROJECT_VERSION}`,
        version,
      },
    };
  }
  try {
    const result = migrated(data, version, options.migrations ?? MIGRATIONS);
    if ('error' in result) return { ok: false, error: result.error };
    const project = projectOf(result.file);
    return version < PROJECT_VERSION ? { ok: true, project, migratedFrom: version } : { ok: true, project };
  } catch (error) {
    // A Damaged is a rule this file breaks; anything else came out of a migration.
    const detail = error instanceof Error ? error.message : String(error);
    return { ok: false, error: { kind: 'damaged', detail, version } };
  }
}
