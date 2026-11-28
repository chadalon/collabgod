// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Where each sample lives on this machine.
 *
 * A project names its samples by content (ADR-0008) and never by path, so it can
 * be opened anywhere. This install keeps its own table from content hash to the
 * paths that held that content when it last looked, in its own data folder and
 * never in a project. Opening a project looks each sample up here. An entry is
 * trusted only while the file at its path still has the size and modification
 * time it had when it was hashed; otherwise the file has changed or gone, and the
 * entry is forgotten. A sample with no trusted entry is missing, which the
 * interface shows on the clip. It is not an error.
 *
 * The table is pure data: the app reads the file system and hands the answers
 * in (`Stat`), and writes `encode()` to disk. The sample indexer (#42) fills the
 * table as it scans; until then the app records each file a person adds.
 */

import { isHash, type SampleRef } from '@collabgod/session';

export const LOCATIONS_FORMAT = 'collabgod.sample-locations';
export const LOCATIONS_VERSION = 1;

/** One file on this machine, as it was when it was hashed. */
export interface SampleLocation {
  /** Where the file is, as this machine's file system spells it. Stays on this machine. */
  readonly path: string;
  /** Size in bytes when it was hashed. */
  readonly size: number;
  /** Modification time when it was hashed, in milliseconds since 1970, as the app reads it. */
  readonly modifiedMs: number;
  /** SHA-256 of the file's bytes (ADR-0008). */
  readonly fileHash: string;
  /** SHA-256 of its decoded PCM audio, for WAV and AIFF. */
  readonly audioHash?: string;
}

/** What the file system says is at a path now, or null if there is nothing there. */
export interface FileStat {
  readonly size: number;
  readonly modifiedMs: number;
}

export type Stat = (path: string) => Promise<FileStat | null>;

export type Resolution =
  /**
   * A file on this machine to play. `match` says how: `file` is the same bytes;
   * `audio` is the same samples in a file whose metadata differs, which sounds
   * the same and counts as exact (ADR-0008).
   */
  | { readonly kind: 'found'; readonly path: string; readonly match: 'file' | 'audio' }
  /** Not on this machine, as far as this install knows. Shown on the clip. */
  | { readonly kind: 'missing' };

const isPath = (value: unknown): value is string => typeof value === 'string' && value.length > 0;
const isSize = (value: unknown): value is number =>
  typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
const isTime = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value);

function isLocation(value: unknown): value is SampleLocation {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  const entry = value as Readonly<Record<string, unknown>>;
  const known = ['path', 'size', 'modifiedMs', 'fileHash', 'audioHash'];
  return (
    Object.keys(entry).every((key) => known.includes(key)) &&
    isPath(entry['path']) &&
    isSize(entry['size']) &&
    isTime(entry['modifiedMs']) &&
    isHash(entry['fileHash']) &&
    (!Object.hasOwn(entry, 'audioHash') || isHash(entry['audioHash']))
  );
}

function copy(location: SampleLocation): SampleLocation {
  const { path, size, modifiedMs, fileHash, audioHash } = location;
  return Object.freeze(
    audioHash === undefined ? { path, size, modifiedMs, fileHash } : { path, size, modifiedMs, fileHash, audioHash },
  );
}

const byPath = (a: SampleLocation, b: SampleLocation): number => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0);

/** This install's table from sample content to the files that hold it. */
export class SampleLocations {
  /** By path: a path holds one file at a time. */
  private readonly entries = new Map<string, SampleLocation>();

  private constructor() {}

  static empty(): SampleLocations {
    return new SampleLocations();
  }

  /**
   * Read the table's file. A table that cannot be read is not worth stopping for:
   * it is only a cache of what the file system says, so an unreadable one gives
   * an empty table and every sample is looked for again.
   */
  static decode(text: string): SampleLocations {
    const table = new SampleLocations();
    let data: unknown;
    try {
      data = JSON.parse(text);
    } catch {
      return table;
    }
    if (typeof data !== 'object' || data === null) return table;
    const file = data as Readonly<Record<string, unknown>>;
    if (file['format'] !== LOCATIONS_FORMAT || file['version'] !== LOCATIONS_VERSION) return table;
    const files = file['files'];
    if (!Array.isArray(files)) return table;
    for (const entry of files) if (isLocation(entry)) table.record(entry);
    return table;
  }

  /** The table as the text of its file, one entry per line, in path order. */
  encode(): string {
    const lines = this.all().map((entry) => `    ${JSON.stringify(entry)}`);
    const files = lines.length === 0 ? '[]' : `[\n${lines.join(',\n')}\n  ]`;
    return `{\n  "format": "${LOCATIONS_FORMAT}",\n  "version": ${LOCATIONS_VERSION},\n  "files": ${files}\n}\n`;
  }

  get size(): number {
    return this.entries.size;
  }

  /** Every entry, in path order. */
  all(): readonly SampleLocation[] {
    return [...this.entries.values()].sort(byPath);
  }

  /**
   * Note a file just hashed: the indexer scanned it, or a person added it to the
   * timeline. Replaces whatever was known about that path.
   *
   * @throws Error if the entry is malformed.
   */
  record(location: SampleLocation): void {
    if (!isLocation(location)) throw new Error('invalid sample location');
    this.entries.set(location.path, copy(location));
  }

  /** Forget a path, as when the file there is gone or has changed since it was hashed. */
  forget(path: string): boolean {
    return this.entries.delete(path);
  }

  /**
   * The files that may hold a sample, best first: those with the same bytes,
   * then those with the same audio. Not yet checked against the file system.
   */
  candidates(sample: SampleRef): readonly { readonly location: SampleLocation; readonly match: 'file' | 'audio' }[] {
    const all = this.all();
    const byFile = all.filter((entry) => entry.fileHash === sample.fileHash);
    const byAudio =
      sample.audioHash === undefined
        ? []
        : all.filter((entry) => entry.fileHash !== sample.fileHash && entry.audioHash === sample.audioHash);
    return [
      ...byFile.map((location) => ({ location, match: 'file' as const })),
      ...byAudio.map((location) => ({ location, match: 'audio' as const })),
    ];
  }

  /**
   * Find a file on this machine to play `sample` from. Each candidate is checked
   * with `stat`, best first; one whose file is gone or has a different size or
   * modification time is forgotten, since what is there now may not be what was
   * hashed. Save the table afterwards if `forgotten` is not empty.
   */
  async resolve(
    sample: SampleRef,
    stat: Stat,
  ): Promise<{ readonly resolution: Resolution; readonly forgotten: readonly string[] }> {
    const forgotten: string[] = [];
    for (const { location, match } of this.candidates(sample)) {
      const now = await stat(location.path);
      if (now !== null && now.size === location.size && now.modifiedMs === location.modifiedMs) {
        return { resolution: { kind: 'found', path: location.path, match }, forgotten };
      }
      // Only forget it if nothing has replaced the entry while we waited for the file system.
      if (this.entries.get(location.path) === location) {
        this.entries.delete(location.path);
        forgotten.push(location.path);
      }
    }
    return { resolution: { kind: 'missing' }, forgotten };
  }
}
