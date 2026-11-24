// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Track order as sortable keys.
 *
 * Each track holds a position key, and the track list is the tracks sorted by
 * key, then by id. Moving a track writes a key between the keys of its new
 * neighbours. That makes a reorder a write to one field of one track, so:
 *
 * - it does not depend on where other tracks are, and a track someone else adds
 *   at the same time does not shift it one place off, as a list index would;
 * - it has a revision like any other field, so undo can guard it (ADR-0004).
 *
 * A key is a string of base-36 digits (`0`-`9`, `a`-`z`) that does not end in
 * `0`. Read it as the fraction 0.k1k2k3... in base 36: plain string comparison
 * then agrees with comparing the fractions, and there is always room for
 * another key between two different ones.
 */

import type { SessionView } from './session';
import type { TrackId } from './types';

const DIGITS = '0123456789abcdefghijklmnopqrstuvwxyz';
const BASE = DIGITS.length;
const KEY = /^[0-9a-z]*[1-9a-z]$/;

/** Longer keys are refused. Reaching this takes thousands of moves into one gap. */
export const MAX_POSITION_LENGTH = 1024;

export const isPosition = (value: unknown): value is string =>
  typeof value === 'string' && value.length <= MAX_POSITION_LENGTH && KEY.test(value);

const digit = (key: string, i: number): number => (i < key.length ? DIGITS.indexOf(key.charAt(i)) : 0);

/**
 * A key between `low` and `high` (exclusive). `''` stands for 0 and `null` for 1.
 * Requires `low < high`.
 */
function midpoint(low: string, high: string | null): string {
  if (high !== null) {
    // Copy the digits the two share; `low` reads as zeros past its end.
    let n = 0;
    while (n < high.length && digit(low, n) === digit(high, n)) n += 1;
    if (n > 0) return high.slice(0, n) + midpoint(low.slice(n), high.slice(n));
  }
  const a = digit(low, 0);
  const b = high === null ? BASE : digit(high, 0);
  if (b - a > 1) return DIGITS.charAt(Math.floor((a + b) / 2));
  // Neighbouring first digits. If `high` goes on past its first digit, that digit alone fits.
  if (high !== null && high.length > 1) return high.slice(0, 1);
  // Otherwise keep `low`'s first digit and find room after it.
  return DIGITS.charAt(a) + midpoint(low.slice(1), null);
}

/**
 * A short key after `low`: the first digit that can go up, goes up. Adding
 * tracks one after another at the end then grows keys by one digit every 35
 * tracks, where halving the gap each time would grow them five times as fast.
 */
function keyAfter(low: string): string {
  for (let i = 0; i < low.length; i += 1) {
    const d = digit(low, i);
    if (d < BASE - 1) return low.slice(0, i) + DIGITS.charAt(d + 1);
  }
  return `${low}1`;
}

/** A short key before `high`, the same way round. */
function keyBefore(high: string): string {
  const d = digit(high, 0);
  if (d >= 2) return DIGITS.charAt(d - 1);
  if (d === 1) return high.length > 1 ? '1' : `0${DIGITS.charAt(BASE - 1)}`;
  return `0${keyBefore(high.slice(1))}`;
}

/**
 * A position key strictly between `before` and `after`. `null` means the start
 * or the end of the list. Deterministic: the same arguments give the same key.
 *
 * @throws RangeError if either key is invalid or `before` is not less than `after`.
 */
export function positionBetween(before: string | null, after: string | null): string {
  if (before !== null && !isPosition(before)) throw new RangeError(`invalid position key ${JSON.stringify(before)}`);
  if (after !== null && !isPosition(after)) throw new RangeError(`invalid position key ${JSON.stringify(after)}`);
  if (before !== null && after !== null && before >= after) {
    throw new RangeError(`position ${JSON.stringify(before)} is not before ${JSON.stringify(after)}`);
  }
  if (before === null && after === null) return DIGITS.charAt(BASE / 2);
  if (after === null) return keyAfter(before ?? '');
  if (before === null) return keyBefore(after);
  return midpoint(before, after);
}

/**
 * The position key that puts a track at `index` in the track list, counted from
 * 0 and clamped to the list. Pass `trackId` when moving an existing track, so
 * its own place is left out of the count.
 *
 * Two tracks can share a key when two people add one at the same spot at once.
 * Their order is then by id, and a track placed next to them lands after both.
 */
export function trackPositionAt(view: SessionView, index: number, trackId?: TrackId): string {
  const keys: string[] = [];
  for (const id of view.trackOrder) {
    const track = view.tracks.get(id);
    if (track !== undefined && id !== trackId) keys.push(track.position);
  }
  const at = Math.max(0, Math.min(Number.isFinite(index) ? Math.trunc(index) : keys.length, keys.length));
  const before = at > 0 ? (keys[at - 1] ?? null) : null;
  const after = keys.slice(at).find((key) => before === null || key > before) ?? null;
  return positionBetween(before, after);
}
