// SPDX-License-Identifier: GPL-3.0-or-later

/** Test samples: generated WAV files, or the user's own, identified by content hash. */

import { createHash } from 'node:crypto';
import { mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

/** A mono 16-bit WAV: a sine that fades out, so edits are easy to hear. */
export function writeTone(path: string, hz: number, seconds: number, amplitude = 0.3, sampleRate = 44100): void {
  const n = Math.round(seconds * sampleRate);
  const data = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const t = i / sampleRate;
    const envelope = Math.min(1, i / 64) * Math.exp((-3 * t) / seconds);
    const v = Math.round(amplitude * envelope * Math.sin(2 * Math.PI * hz * t) * 32767);
    data.writeInt16LE(v, i * 2);
  }
  const header = Buffer.alloc(44);
  header.write('RIFF', 0, 'ascii');
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVE', 8, 'ascii');
  header.write('fmt ', 12, 'ascii');
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(1, 22); // mono
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(sampleRate * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36, 'ascii');
  header.writeUInt32LE(data.length, 40);
  writeFileSync(path, Buffer.concat([header, data]));
}

export const hashFile = (path: string): string => createHash('sha256').update(readFileSync(path)).digest('hex');

export interface SampleLibrary {
  /** Sample hash to absolute file path, on this machine only. */
  readonly files: ReadonlyMap<string, string>;
  readonly hashes: readonly string[];
}

/** Writes a few tones into `folder` and indexes them. */
export function generatedLibrary(folder: string): SampleLibrary {
  mkdirSync(folder, { recursive: true });
  const files = new Map<string, string>();
  for (const hz of [330, 440 * 1.25, 660, 880]) {
    const path = resolve(join(folder, `tone-${Math.round(hz)}.wav`));
    writeTone(path, hz, 1.0);
    files.set(hashFile(path), path);
  }
  return { files, hashes: [...files.keys()] };
}

/** Indexes up to `limit` .wav and .aif files directly in `folder`. */
export function folderLibrary(folder: string, limit = 32): SampleLibrary {
  const files = new Map<string, string>();
  for (const name of readdirSync(folder).sort()) {
    if (!/\.(wav|aiff?)$/i.test(name)) continue;
    const path = resolve(join(folder, name));
    files.set(hashFile(path), path);
    if (files.size >= limit) break;
  }
  if (files.size === 0) throw new Error(`no .wav or .aif files in ${folder}`);
  return { files, hashes: [...files.keys()] };
}
