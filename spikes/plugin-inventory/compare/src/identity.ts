// The proposed plugin identity rule. Spike code (issue #14).
//
// The starting rule, from the issue:
//   - same class ID and version: identical;
//   - same class ID, different version: usable but flagged (PLG-7);
//   - different class ID: a different plugin.
//
// Whether a version difference should block rather than flag is the
// maintainer's decision, informed by #12 (parameter sync) and #13 (state sync).
// It is a parameter here, not a choice made by this code.

import type { Plugin } from './inventory.js';

export type VersionPolicy = 'flag' | 'block';

export type Verdict =
  /** Same class ID, same version text. */
  | 'identical'
  /** Same class ID; the version texts differ only in form, such as 1.2 and 1.2.0. */
  | 'equivalent-version'
  /** Same class ID, different versions. */
  | 'version-differs';

export interface Judgement {
  verdict: Verdict;
  /** May the lobby offer this plugin? */
  usable: boolean;
  /** Should the interface warn before it is used? (PLG-7) */
  flagged: boolean;
}

/**
 * Normalises a version for comparison: trims, drops a leading "v", drops
 * leading zeros in numeric parts and trailing ".0" parts. "v1.02.0" and "1.2"
 * become "1.2". Anything not made of dot-separated numbers is only trimmed and
 * lower-cased, so "1.2 beta" stays distinct from "1.2".
 */
export function normaliseVersion(v: string): string {
  const t = v.trim().replace(/^v(?=\d)/i, '');
  if (!/^\d+(\.\d+)*$/.test(t)) return t.toLowerCase();
  const parts = t.split('.').map((p) => String(Number.parseInt(p, 10)));
  while (parts.length > 1 && parts[parts.length - 1] === '0') parts.pop();
  return parts.join('.');
}

/** The identity key. Two plugins with different keys are different plugins. */
export function identityKey(p: Pick<Plugin, 'cid'>): string {
  return p.cid;
}

/** Judges two installs with the same class ID. */
export function judge(a: Pick<Plugin, 'version'>, b: Pick<Plugin, 'version'>, policy: VersionPolicy = 'flag'): Judgement {
  if (a.version === b.version) return { verdict: 'identical', usable: true, flagged: false };
  if (normaliseVersion(a.version) === normaliseVersion(b.version)) {
    return { verdict: 'equivalent-version', usable: true, flagged: false };
  }
  return { verdict: 'version-differs', usable: policy === 'flag', flagged: true };
}

/**
 * The version of the release a plugin came from, as the installer stamped it
 * outside the factory: CFBundleShortVersionString on macOS, the product version
 * resource on Windows, or else moduleinfo.json's module version. Null when the
 * bundle says nothing. Used only to spot plugins whose factory version is stale.
 */
export function releaseVersion(p: Pick<Plugin, 'binaryVersion' | 'moduleInfo'>): string | null {
  const bv = p.binaryVersion;
  const fromBinary = bv?.['shortVersion'] ?? bv?.['productVersion'];
  if (fromBinary !== undefined && fromBinary !== '' && fromBinary !== '0.0.0.0') return fromBinary;
  if (p.moduleInfo && p.moduleInfo.version !== '') return p.moduleInfo.version;
  return null;
}

/** For spotting the same plugin under two IDs: case, accents' encoding, spacing and punctuation ignored. */
export function looseName(vendor: string, name: string): string {
  const squash = (s: string) => s.normalize('NFC').toLowerCase().replace(/[^\p{L}\p{N}]+/gu, '');
  return `${squash(vendor)}/${squash(name)}`;
}
