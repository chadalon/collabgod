// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { type LoadErrorKind, loadProject, type Migration, PROJECT_VERSION, saveProject } from '../src';
import { hash, loaded, smallProject, stateOf } from './helpers';

const text = saveProject(smallProject());
const file = (): Record<string, unknown> => JSON.parse(text);
const write = (data: unknown): string => JSON.stringify(data);

/** The saved file with one thing changed, as text. */
function edited(change: (data: Record<string, any>) => void): string {
  const data = file();
  change(data);
  return write(data);
}

function refused(input: string, kind: LoadErrorKind, detail?: RegExp): void {
  const result = loadProject(input);
  expect(result.ok).toBe(false);
  if (result.ok) return;
  expect(result.error.kind).toBe(kind);
  if (detail !== undefined) expect(result.error.detail).toMatch(detail);
}

describe('loading', () => {
  it('reads what was saved, with a byte order mark or Windows line endings too', () => {
    const state = stateOf(smallProject());
    expect(stateOf(loaded(loadProject(text)))).toBe(state);
    expect(stateOf(loaded(loadProject(`\uFEFF${text}`)))).toBe(state);
    expect(stateOf(loaded(loadProject(text.replaceAll('\n', '\r\n'))))).toBe(state);
  });

  it('says when a file is not a project at all', () => {
    refused('', 'not-a-project');
    refused('{"format": "collabgod.proj', 'not-a-project');
    refused('[]', 'not-a-project');
    refused(write({ ...file(), format: 'something.else' }), 'not-a-project');
    refused(write({ version: 1 }), 'not-a-project');
  });

  it('says when a file is from a newer version of the app, and which version', () => {
    const result = loadProject(write({ ...file(), version: PROJECT_VERSION + 1 }));
    expect(result).toMatchObject({ ok: false, error: { kind: 'newer-version', version: PROJECT_VERSION + 1 } });
  });

  it('says when an old version cannot be migrated', () => {
    refused(write({ ...file(), version: 0 }), 'unsupported-version', /version 0/);
    refused(write({ ...file(), version: -3 }), 'unsupported-version');
    refused(write({ ...file(), version: '1' }), 'damaged');
  });

  it('refuses a field the format does not define, anywhere, and a field it needs that is missing', () => {
    const extra = (where: (data: Record<string, any>) => Record<string, unknown>, key: string): string =>
      edited((data) => {
        where(data)[key] = 'x';
      });
    refused(extra((data) => data, 'path'), 'damaged', /unknown field path/);
    refused(extra((data) => data['timeline'], 'extra'), 'damaged', /timeline/);
    refused(extra((data) => data['members'][0], 'secret'), 'damaged', /member 0/);
    refused(extra((data) => data['members'][0].verifier, 'secret'), 'damaged', /member 0 verifier/);
    refused(extra((data) => data['tracks'][0], 'colour'), 'damaged', /track 0/);
    refused(extra((data) => data['clips'][0], 'muted'), 'damaged', /clip 0/);
    refused(extra((data) => data['clips'][0].rev, 'muted'), 'damaged', /clip 0 rev/);
    for (const key of ['projectId', 'seq', 'timeline', 'members', 'tracks', 'clips']) {
      refused(
        edited((data) => {
          delete data[key];
        }),
        'damaged',
        new RegExp(key),
      );
    }
  });

  it('never takes a local path into the session, even inside a sample', () => {
    const data = file() as { clips: { sample: Record<string, unknown> }[] };
    const clip = data.clips[0];
    if (clip === undefined) throw new Error('no clip');
    clip.sample['path'] = 'C:\\Samples\\kick.wav';
    refused(write(data), 'damaged', /sample/);
    clip.sample = { fileHash: hash(1), name: 'C:\\Samples\\kick.wav' };
    refused(write(data), 'damaged');
  });

  it('checks every value and rule as the model does', () => {
    const with_ = edited;
    refused(with_((data) => (data['timeline'].tempo = 5)), 'damaged', /timeline/);
    refused(with_((data) => (data['clips'][0].trackId = 'gone')), 'damaged', /missing track/);
    refused(with_((data) => (data['clips'][0].length = 0)), 'damaged', /clip/);
    refused(with_((data) => data['clips'].push(data['clips'][0])), 'damaged', /twice/);
    refused(with_((data) => (data['members'][0].verifier.value = 'hunter2')), 'damaged', /member/);
    refused(with_((data) => (data['members'][0].createdBy = '')), 'damaged', /member/);
    refused(with_((data) => (data['tracks'][0].by = { name: 'chad' })), 'damaged', /track 0 by/);
    refused(with_((data) => (data['seq'] = -1)), 'damaged', /seq/);
    refused(with_((data) => (data['projectId'] = '')), 'damaged', /project id/);
    refused(with_((data) => (data['projectId'] = 7)), 'damaged', /project id/);
    refused(with_((data) => (data['history'].member = '')), 'damaged', /history/);
    refused(with_((data) => (data['history'].undo[0].inverse = 'undo')), 'damaged', /history/);
  });

  it('opens a project whose samples are nowhere on this machine: finding them is not loading’s job', () => {
    // Every sample in the small project has a made-up hash, so none can be on disk.
    expect(loadProject(text).ok).toBe(true);
  });
});

describe('migrating', () => {
  /**
   * A made-up version 0, before revisions had authors: `by` and `createdBy` did
   * not exist, and the timeline sat in two top-level fields. Its migration fills
   * in an author nobody can be blamed for, which a real migration would document.
   */
  function versionZero(): Record<string, unknown> {
    const { timeline, members, tracks, clips, history: _history, ...rest } = file() as Record<string, any>;
    const strip = ({ by: _by, createdBy: _createdBy, ...object }: Record<string, unknown>) => object;
    return {
      ...rest,
      version: 0,
      tempo: timeline.tempo,
      timeSignature: timeline.timeSignature,
      timelineRev: timeline.rev,
      members: members.map(strip),
      tracks: tracks.map(strip),
      clips: clips.map(strip),
    };
  }

  const unknown = 'unknown';
  const authored = (object: Record<string, any>) => ({
    ...object,
    createdBy: unknown,
    by: Object.fromEntries(Object.keys(object['rev']).map((field) => [field, unknown])),
  });
  const zeroToOne: Migration = {
    from: 0,
    migrate: ({ tempo, timeSignature, timelineRev, ...rest }: Record<string, any>) => ({
      ...rest,
      version: 1,
      timeline: { tempo, timeSignature, rev: timelineRev, by: { tempo: unknown, timeSignature: unknown } },
      members: rest['members'].map(authored),
      tracks: rest['tracks'].map(authored),
      clips: rest['clips'].map(authored),
    }),
  };

  it('brings an older file up to the current version on load, and says it did', () => {
    const result = loadProject(write(versionZero()), { migrations: [zeroToOne] });
    expect(result).toMatchObject({ ok: true, migratedFrom: 0 });
    const project = loaded(result);
    expect(project.session.clips.map((clip) => clip.id)).toEqual(['k1', 'k2', 'k3']);
    expect(project.session.clips[0]?.createdBy).toBe(unknown);
    expect(project.session.timeline.tempo).toBe(92.5);
    // Saved again, it is a current file.
    expect(JSON.parse(saveProject(project)).version).toBe(PROJECT_VERSION);
  });

  it('runs every step in order, one version at a time', () => {
    const minusOneToZero: Migration = {
      from: -1,
      migrate: ({ beats, ...rest }: Record<string, any>) => ({ ...rest, version: 0, tempo: beats }),
    };
    const { tempo, ...older } = versionZero();
    const result = loadProject(write({ ...older, version: -1, beats: tempo }), {
      migrations: [zeroToOne, minusOneToZero],
    });
    expect(result).toMatchObject({ ok: true, migratedFrom: -1 });
  });

  it('calls a file damaged when a migration fails or skips a version, and never half loads it', () => {
    const throws: Migration = { from: 0, migrate: () => { throw new Error('boom'); } };
    const skips: Migration = { from: 0, migrate: (data) => ({ ...data, version: 2 }) };
    for (const migration of [throws, skips]) {
      const result = loadProject(write(versionZero()), { migrations: [migration] });
      expect(result).toMatchObject({ ok: false, error: { kind: 'damaged', version: 0 } });
    }
  });

  it('checks a migrated file exactly as a current one', () => {
    const careless: Migration = { from: 0, migrate: (data) => ({ ...data, version: 1 }) };
    const result = loadProject(write(versionZero()), { migrations: [careless] });
    expect(result).toMatchObject({ ok: false, error: { kind: 'damaged' } });
  });
});
