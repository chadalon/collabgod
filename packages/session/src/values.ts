// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Units, limits and the rules every value in the session must follow.
 *
 * Operations arrive from the network, so nothing here trusts TypeScript's types:
 * each check takes `unknown` and looks at the actual value.
 */

import type { ClipValues, NewMember, NewTrack, SampleRef, TimeSignature } from './types';

/** Positions and lengths are in ticks: 960 to a quarter note. */
export const TICKS_PER_QUARTER = 960;

export const DEFAULT_TEMPO = 120;
export const DEFAULT_TIME_SIGNATURE: TimeSignature = Object.freeze({ numerator: 4, denominator: 4 });
export const MIN_TEMPO = 20;
export const MAX_TEMPO = 999;
export const MAX_TIME_SIGNATURE_NUMERATOR = 32;
export const TIME_SIGNATURE_DENOMINATORS: readonly number[] = Object.freeze([1, 2, 4, 8, 16, 32]);

/** Clip gain range. A user interface may show the minimum as silence. */
export const MIN_CLIP_GAIN_DB = -96;
export const MAX_CLIP_GAIN_DB = 24;

export const MAX_ID_LENGTH = 256;
export const MAX_TRACK_NAME_LENGTH = 256;
export const MAX_MEMBER_NAME_LENGTH = 64;
export const MAX_SAMPLE_NAME_LENGTH = 255;

/** Seconds that `ticks` last at `tempo` quarter notes per minute. */
export function ticksToSeconds(ticks: number, tempo: number): number {
  return (ticks * 60) / (tempo * TICKS_PER_QUARTER);
}

/**
 * The fades as they should sound. Stored fades may add up to more than the
 * clip, for instance after the clip was trimmed; then both shrink in proportion
 * so they meet. The interface and the engine both use this, so they agree.
 */
export function effectiveFades(clip: Pick<ClipValues, 'length' | 'fadeIn' | 'fadeOut'>): {
  readonly fadeIn: number;
  readonly fadeOut: number;
} {
  const total = clip.fadeIn + clip.fadeOut;
  if (total <= clip.length) return { fadeIn: clip.fadeIn, fadeOut: clip.fadeOut };
  const fadeIn = Math.floor((clip.length * clip.fadeIn) / total);
  return { fadeIn, fadeOut: clip.length - fadeIn };
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

export type Plain = Readonly<Record<string, unknown>>;

export const isObject = (value: unknown): value is Plain =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

export const has = (record: object, key: string): boolean => Object.hasOwn(record, key);

/** True if `value` is an object whose own keys are all in `allowed`. */
export const onlyKeys = (value: Plain, allowed: readonly string[]): boolean =>
  Object.keys(value).every((key) => allowed.includes(key));

export const isId = (value: unknown): value is string =>
  typeof value === 'string' && value.length > 0 && value.length <= MAX_ID_LENGTH;

/** A position or a fade length: a whole number of ticks, zero or more. */
export const isTick = (value: unknown): value is number =>
  typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;

/** A clip length: a whole number of ticks, more than zero. */
export const isLength = (value: unknown): value is number => isTick(value) && value > 0;

export const isOffset = (value: unknown): value is number =>
  typeof value === 'number' && Number.isFinite(value) && value >= 0;

export const isGain = (value: unknown): value is number =>
  typeof value === 'number' && Number.isFinite(value) && value >= MIN_CLIP_GAIN_DB && value <= MAX_CLIP_GAIN_DB;

export const isTempo = (value: unknown): value is number =>
  typeof value === 'number' && Number.isFinite(value) && value >= MIN_TEMPO && value <= MAX_TEMPO;

export const isTimeSignature = (value: unknown): value is TimeSignature =>
  isObject(value) &&
  onlyKeys(value, ['numerator', 'denominator']) &&
  typeof value['numerator'] === 'number' &&
  Number.isInteger(value['numerator']) &&
  value['numerator'] >= 1 &&
  value['numerator'] <= MAX_TIME_SIGNATURE_NUMERATOR &&
  typeof value['denominator'] === 'number' &&
  TIME_SIGNATURE_DENOMINATORS.includes(value['denominator']);

export const isTrackName = (value: unknown): value is string =>
  typeof value === 'string' && value.length <= MAX_TRACK_NAME_LENGTH;

export const isMemberName = (value: unknown): value is string =>
  typeof value === 'string' && value.trim().length > 0 && value.length <= MAX_MEMBER_NAME_LENGTH;

export const isColour = (value: unknown): value is string => typeof value === 'string' && /^#[0-9a-f]{6}$/.test(value);

/** SHA-256 as 64 lowercase hex digits, as the sample indexer writes it. */
export const isHash = (value: unknown): value is string => typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);

/**
 * A file name with no folder in it. Refusing separators keeps a local path out
 * of the session even by accident (AGENTS.md: a local path never goes into a
 * session or over the network).
 */
export const isSampleName = (value: unknown): value is string =>
  typeof value === 'string' &&
  value.length > 0 &&
  value.length <= MAX_SAMPLE_NAME_LENGTH &&
  !value.includes('/') &&
  !value.includes('\\');

/** Only the three known keys, so nothing else (a path, say) can ride along into the session. */
export const isSampleRef = (value: unknown): value is SampleRef =>
  isObject(value) &&
  onlyKeys(value, ['fileHash', 'audioHash', 'name']) &&
  isHash(value['fileHash']) &&
  (!has(value, 'audioHash') || isHash(value['audioHash'])) &&
  isSampleName(value['name']);

export const isNewMember = (value: unknown): value is NewMember =>
  isObject(value) && isId(value['id']) && isMemberName(value['name']) && isColour(value['colour']);

export const isNewTrack = (value: unknown, isPosition: (key: unknown) => boolean): value is NewTrack =>
  isObject(value) && isId(value['id']) && isTrackName(value['name']) && isPosition(value['position']);

/** Checks for each clip field on its own. Whether `trackId` names a real track is the session's question. */
export const CLIP_FIELD_CHECKS: { readonly [K in keyof ClipValues]: (value: unknown) => boolean } = {
  trackId: isId,
  start: isTick,
  length: isLength,
  offset: isOffset,
  gain: isGain,
  fadeIn: isTick,
  fadeOut: isTick,
};
