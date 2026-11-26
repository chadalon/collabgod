// SPDX-License-Identifier: GPL-3.0-or-later

import { canonical, Session } from '@collabgod/session';
import { describe, expect, it } from 'vitest';
import { loadProject, PROJECT_FORMAT, PROJECT_VERSION, type Project, replicaFor, saveProject } from '../src';
import { hash, loaded, smallProject, solo, stateOf } from './helpers';

const empty: Project = { seq: 0, session: Session.empty().snapshot() };

describe('saving', () => {
  it('writes the format and its version first, so any version of the app can tell what it is holding', () => {
    const text = saveProject(empty);
    expect(text.split('\n').slice(0, 4)).toEqual([
      '{',
      `  "format": "${PROJECT_FORMAT}",`,
      `  "version": ${PROJECT_VERSION},`,
      '  "seq": 0,',
    ]);
    expect(text.endsWith('}\n')).toBe(true);
  });

  it('puts one object on each line, so a change to one clip changes one line', () => {
    const text = saveProject(smallProject());
    const clipLines = text.split('\n').filter((line) => line.startsWith('    {"id":"k'));
    expect(clipLines).toHaveLength(3);
    for (const line of clipLines) expect(() => JSON.parse(line.replace(/,$/, ''))).not.toThrow();
  });

  it('gives the same text for the same project, however often it is saved and loaded', () => {
    const text = saveProject(smallProject());
    expect(saveProject(smallProject())).toBe(text);
    const again = saveProject(loaded(loadProject(text)));
    expect(again).toBe(text);
  });

  it('holds positions in ticks with the tempo and time signature, and each clip’s offset into its file', () => {
    const file = JSON.parse(saveProject(smallProject()));
    expect(file.timeline).toMatchObject({ tempo: 92.5, timeSignature: { numerator: 7, denominator: 8 } });
    const k3 = file.clips.find((clip: { id: string }) => clip.id === 'k3');
    // Trimmed a quarter of a second into its file: the start moves in whole ticks, the offset is in seconds.
    expect(k3.start).toBe(Math.round((0.25 * 92.5 * 960) / 60));
    expect(k3.offset).toBeCloseTo(0.25, 3);
    expect(Number.isInteger(k3.start) && Number.isInteger(k3.length)).toBe(true);
  });

  it('names each sample by file hash, audio hash when known, and a name, and nothing else', () => {
    const file = JSON.parse(saveProject(smallProject()));
    const samples = file.clips.map((clip: { sample: object }) => clip.sample);
    for (const sample of samples) {
      expect(['fileHash', 'name', 'audioHash']).toEqual(expect.arrayContaining(Object.keys(sample)));
    }
    expect(samples.find((sample: { name: string }) => sample.name === 'sub.aif')).toEqual({
      fileHash: hash(3),
      audioHash: hash(33),
      name: 'sub.aif',
    });
  });

  it('holds the member list: id, name, colour, removed, and what checks the secret, never the secret', () => {
    const file = JSON.parse(saveProject(smallProject()));
    expect(file.members).toHaveLength(1);
    const [member] = file.members;
    expect(Object.keys(member)).toEqual(['id', 'name', 'colour', 'removed', 'verifier', 'createdBy', 'rev', 'by']);
    expect(member).toMatchObject({ id: 'chad', name: 'Chad', colour: '#ff8800', removed: false });
    expect(Object.keys(member.verifier)).toEqual(['scheme', 'value']);
  });

  it('records who made what: who added each object, and who last wrote each field', () => {
    const { me, edit, send } = solo('chad');
    edit({ type: 'track.add', track: { id: 't1', name: 'Drums', position: 'i' } });
    // Someone else's edit, ordered after Chad's, as the relay would deliver it.
    send([]);
    const rename = { type: 'track.rename', trackId: 't1', name: 'Kit' } as const;
    me.receive({ seq: me.seq + 1, operation: { id: 'sam:1', author: 'sam', change: rename } });
    const file = JSON.parse(saveProject({ seq: me.seq, session: me.confirmedSnapshot() }));
    expect(file.tracks[0]).toMatchObject({ createdBy: 'chad', by: { name: 'sam', position: 'chad' } });
  });

  it('holds the undo history of the member who saved it, and leaves it out when there is none', () => {
    const project = smallProject();
    const file = JSON.parse(saveProject(project));
    expect(file.history.member).toBe('chad');
    expect(file.history.undo.length).toBeGreaterThan(0);
    expect(file.history.redo).toHaveLength(1);
    expect(Object.keys(JSON.parse(saveProject(empty)))).not.toContain('history');
  });

  it('refuses a seq that is not a whole number', () => {
    expect(() => saveProject({ ...empty, seq: -1 })).toThrow();
    expect(() => saveProject({ ...empty, seq: 1.5 })).toThrow();
  });
});

describe('reopening', () => {
  it('gives the same model state that was saved', () => {
    const project = smallProject();
    expect(stateOf(loaded(loadProject(saveProject(project))))).toBe(stateOf(project));
  });

  it('lets the member who saved it undo and redo where they left off', () => {
    const project = loaded(loadProject(saveProject(smallProject())));
    const me = replicaFor(project, { author: 'chad', nextOpId: () => 'chad:run2:1' });
    expect(me.canRedo).toBe(true);
    const redo = me.redo();
    expect(redo.applied).toBe(true);
    expect(me.view.clips.get('k2')?.gain).toBe(-4.5);
  });

  it('gives another member an empty history, and the same state', () => {
    const project = loaded(loadProject(saveProject(smallProject())));
    const sam = replicaFor(project, { author: 'sam', nextOpId: () => 'sam:1' });
    expect(sam.canUndo || sam.canRedo).toBe(false);
    expect(canonical(sam.view)).toBe(canonical(Session.fromSnapshot(project.session)));
  });
});
