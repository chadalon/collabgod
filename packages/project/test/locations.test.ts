// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { type FileStat, loadProject, projectOf, type SampleLocation, SampleLocations, saveProject } from '../src';
import { hash, solo } from './helpers';

const kick: SampleLocation = {
  path: '/Users/chad/Samples/kick.wav',
  size: 1000,
  modifiedMs: 1_700_000_000_000,
  fileHash: hash(1),
};
const kickCopy: SampleLocation = { ...kick, path: '/Users/chad/Backup/kick.wav' };
const subWav: SampleLocation = {
  path: 'D:\\Samples\\sub.wav',
  size: 5000,
  modifiedMs: 1_700_000_000_500,
  fileHash: hash(4),
  audioHash: hash(33),
};

/** A file system holding exactly these files, as they are now. */
function disk(files: readonly { path: string; size: number; modifiedMs: number }[]) {
  const asked: string[] = [];
  const stat = async (path: string): Promise<FileStat | null> => {
    asked.push(path);
    const file = files.find((entry) => entry.path === path);
    return file === undefined ? null : { size: file.size, modifiedMs: file.modifiedMs };
  };
  return { stat, asked };
}

function table(...entries: SampleLocation[]): SampleLocations {
  const locations = SampleLocations.empty();
  for (const entry of entries) locations.record(entry);
  return locations;
}

describe('the sample location table', () => {
  it('finds a sample by its file hash when the file is still as it was hashed', async () => {
    const { stat } = disk([kick]);
    const { resolution, forgotten } = await table(kick).resolve({ fileHash: hash(1), name: 'kick.wav' }, stat);
    expect(resolution).toEqual({ kind: 'found', path: kick.path, match: 'file' });
    expect(forgotten).toEqual([]);
  });

  it('forgets a path whose file is gone or has changed, and tries the next one', async () => {
    const changed = { ...kickCopy, size: 999 };
    const { stat, asked } = disk([changed]);
    const locations = table(kick, kickCopy);
    const { resolution, forgotten } = await locations.resolve({ fileHash: hash(1), name: 'kick.wav' }, stat);
    expect(resolution).toEqual({ kind: 'missing' });
    expect(asked).toEqual([kickCopy.path, kick.path]);
    expect([...forgotten].sort()).toEqual([kick.path, kickCopy.path].sort());
    expect(locations.size).toBe(0);
  });

  it('plays from a file with the same audio when no file has the same bytes (ADR-0008)', async () => {
    const { stat } = disk([subWav]);
    const sample = { fileHash: hash(3), audioHash: hash(33), name: 'sub.aif' };
    const { resolution } = await table(subWav).resolve(sample, stat);
    expect(resolution).toEqual({ kind: 'found', path: subWav.path, match: 'audio' });
  });

  it('prefers the same bytes over the same audio', () => {
    const exact = { ...subWav, path: '/exact.wav', fileHash: hash(3) };
    const sample = { fileHash: hash(3), audioHash: hash(33), name: 'sub.aif' };
    expect(table(subWav, exact).candidates(sample).map((c) => c.match)).toEqual(['file', 'audio']);
  });

  it('says a sample it does not know is missing, which is not an error', async () => {
    const { stat, asked } = disk([kick]);
    const { resolution } = await table(kick).resolve({ fileHash: hash(9), name: 'nowhere.wav' }, stat);
    expect(resolution).toEqual({ kind: 'missing' });
    expect(asked).toEqual([]);
  });

  it('keeps one entry per path, the latest', () => {
    const locations = table(kick, { ...kick, fileHash: hash(2), size: 2 });
    expect(locations.all()).toEqual([{ ...kick, fileHash: hash(2), size: 2 }]);
  });

  it('is saved and read back, one entry per line, in path order', () => {
    const locations = table(kick, subWav, kickCopy);
    const text = locations.encode();
    expect(text.split('\n').filter((line) => line.startsWith('    {'))).toHaveLength(3);
    expect(SampleLocations.decode(text).all()).toEqual(locations.all());
    expect(SampleLocations.decode(text).encode()).toBe(text);
  });

  it('reads an unreadable table as empty, and skips malformed entries', () => {
    expect(SampleLocations.decode('not json').size).toBe(0);
    expect(SampleLocations.decode('{"format":"other","version":1,"files":[]}').size).toBe(0);
    const text = JSON.stringify({
      format: 'collabgod.sample-locations',
      version: 1,
      files: [
        kick,
        { ...kick, path: '' },
        { ...kick, path: '/a', fileHash: 'nope' },
        { ...kick, path: '/b', extra: 1 },
      ],
    });
    expect(SampleLocations.decode(text).all()).toEqual([kick]);
    expect(() => SampleLocations.empty().record({ ...kick, size: -1 })).toThrow();
  });

  it('never puts a path into the project: the project names samples by content only', () => {
    const { me, edit } = solo();
    edit({ type: 'track.add', track: { id: 't1', name: 'Drums', position: 'i' } });
    edit({
      type: 'clip.add',
      clip: {
        id: 'k1',
        trackId: 't1',
        start: 0,
        length: 960,
        offset: 0,
        gain: 0,
        fadeIn: 0,
        fadeOut: 0,
        sample: { fileHash: kick.fileHash, name: 'kick.wav' },
      },
    });
    const text = saveProject(projectOf(me));
    expect(text).not.toContain('/Users/');
    expect(text).not.toContain('Samples');
    expect(loadProject(text).ok).toBe(true);
  });
});
