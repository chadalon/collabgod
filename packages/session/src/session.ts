// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The session: one mutable state that changes only through `apply`.
 *
 * `apply` is deterministic: the same state and the same operation always give
 * the same result, on every machine. It never throws on an operation it cannot
 * apply; it leaves the state as it was and says why. When it does apply, it
 * returns the inverse: a guarded change that takes the state back exactly. That
 * is how a replica rolls back its unconfirmed edits in time proportional to how
 * many there are, not to the size of the session (ADR-0014).
 *
 * The objects the session hands out (tracks, clips, members, the timeline) are
 * frozen and replaced, never changed, so a reader can keep one and compare it
 * with a later one by identity.
 */

import { isPosition } from './position';
import {
  type ApplyResult,
  type Change,
  type Clip,
  type ClipField,
  type ClipId,
  type ClipValues,
  type FieldPatch,
  type Member,
  type MemberField,
  type MemberId,
  type MemberValues,
  type NewClip,
  NO_REVISION,
  type OpId,
  type Operation,
  type RejectReason,
  type Revisions,
  type SampleRef,
  type Timeline,
  type TimelineField,
  type TimelineValues,
  type TimeSignature,
  type Track,
  type TrackField,
  type TrackId,
  type TrackValues,
} from './types';
import {
  CLIP_FIELD_CHECKS,
  DEFAULT_TEMPO,
  DEFAULT_TIME_SIGNATURE,
  has,
  isColour,
  isGain,
  isId,
  isLength,
  isMemberName,
  isNewMember,
  isNewTrack,
  isObject,
  isSampleRef,
  isTempo,
  isTick,
  isTimeSignature,
  isTrackName,
  MAX_ID_LENGTH,
  onlyKeys,
  type Plain,
  ticksToSeconds,
} from './values';

const TIMELINE_FIELDS: readonly TimelineField[] = ['tempo', 'timeSignature'];
const MEMBER_FIELDS: readonly MemberField[] = ['name', 'colour', 'removed'];
const TRACK_FIELDS: readonly TrackField[] = ['name', 'position'];
const CLIP_FIELDS: readonly ClipField[] = ['trackId', 'start', 'length', 'offset', 'gain', 'fadeIn', 'fadeOut'];
const SPLIT_FIELDS = ['length', 'fadeOut'] as const;

/** Seconds. A trim that lands this close to the start of the file is at the start of the file. */
const OFFSET_ROUNDING = 1e-9;

type Checks<F extends string> = { readonly [K in F]: (value: unknown) => boolean };

const TIMELINE_CHECKS: Checks<TimelineField> = { tempo: isTempo, timeSignature: isTimeSignature };
const MEMBER_CHECKS: Checks<MemberField> = {
  name: isMemberName,
  colour: isColour,
  removed: (value) => typeof value === 'boolean',
};
const TRACK_CHECKS: Checks<TrackField> = { name: isTrackName, position: isPosition };

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/** What the interface, the engine projection and the project file read. It cannot change the session. */
export interface SessionView {
  readonly timeline: Timeline;
  readonly members: ReadonlyMap<MemberId, Member>;
  readonly tracks: ReadonlyMap<TrackId, Track>;
  /** Track ids in the order they are shown: by position key, then by id. */
  readonly trackOrder: readonly TrackId[];
  readonly clips: ReadonlyMap<ClipId, Clip>;
  /** The clips on one track, by start, then by id. */
  clipsOnTrack(trackId: TrackId): readonly Clip[];
  snapshot(): SessionSnapshot;
  /**
   * Structural rules every reachable state keeps. Returns what is broken, if
   * anything. For tests; costs time in proportion to the session's size.
   */
  checkInvariants(): string[];
}

/** The whole session as plain data, in a stable order. Revisions included. */
export interface SessionSnapshot {
  readonly timeline: Timeline;
  /** By id. */
  readonly members: readonly Member[];
  /** In track order. */
  readonly tracks: readonly Track[];
  /** By id. */
  readonly clips: readonly Clip[];
}

// ---------------------------------------------------------------------------
// Building frozen objects. Every object is built field by field, so nothing an
// operation carries besides the known fields (a stray path, say) gets in.
// ---------------------------------------------------------------------------

function revisions<F extends string>(fields: readonly F[], revFor: (field: F) => OpId): Revisions<F> {
  const rev: Partial<Record<F, OpId>> = {};
  for (const field of fields) rev[field] = revFor(field);
  return Object.freeze(rev as Record<F, OpId>);
}

const all = (opId: OpId) => (): OpId => opId;

function makeTimeSignature(value: TimeSignature): TimeSignature {
  return Object.freeze({ numerator: value.numerator, denominator: value.denominator });
}

function makeTimeline(values: TimelineValues, rev: Revisions<TimelineField>): Timeline {
  return Object.freeze({
    tempo: values.tempo,
    timeSignature: makeTimeSignature(values.timeSignature),
    rev: revisions(TIMELINE_FIELDS, (field) => rev[field]),
  });
}

function makeMember(id: MemberId, values: MemberValues, rev: Revisions<MemberField>): Member {
  return Object.freeze({
    id,
    name: values.name,
    colour: values.colour,
    removed: values.removed,
    rev: revisions(MEMBER_FIELDS, (field) => rev[field]),
  });
}

function makeTrack(id: TrackId, values: TrackValues, rev: Revisions<TrackField>): Track {
  return Object.freeze({
    id,
    name: values.name,
    position: values.position,
    rev: revisions(TRACK_FIELDS, (field) => rev[field]),
  });
}

function makeSample(sample: SampleRef): SampleRef {
  return Object.freeze(
    sample.audioHash === undefined
      ? { fileHash: sample.fileHash, name: sample.name }
      : { fileHash: sample.fileHash, audioHash: sample.audioHash, name: sample.name },
  );
}

function makeClip(id: ClipId, sample: SampleRef, values: ClipValues, rev: Revisions<ClipField>): Clip {
  return Object.freeze({
    id,
    sample: makeSample(sample),
    trackId: values.trackId,
    start: values.start,
    length: values.length,
    offset: values.offset,
    gain: values.gain,
    fadeIn: values.fadeIn,
    fadeOut: values.fadeOut,
    rev: revisions(CLIP_FIELDS, (field) => rev[field]),
  });
}

/** A clip without its revisions, as `clip.add` carries it. */
function newClipOf(clip: Clip): NewClip {
  const { rev: _rev, ...values } = clip;
  return values;
}

// ---------------------------------------------------------------------------
// Comparing
// ---------------------------------------------------------------------------

const sameValue = (a: unknown, b: unknown): boolean =>
  isObject(a) && isObject(b) ? a['numerator'] === b['numerator'] && a['denominator'] === b['denominator'] : a === b;

const sameFields = <F extends string>(
  fields: readonly F[],
  a: { readonly [K in F]: unknown },
  b: { readonly [K in F]: unknown },
): boolean => fields.every((field) => sameValue(a[field], b[field]));

const sameSample = (a: SampleRef, b: SampleRef): boolean =>
  a.fileHash === b.fileHash && a.audioHash === b.audioHash && a.name === b.name;

const sameMember = (a: Member, b: Member): boolean =>
  a.id === b.id && sameFields(MEMBER_FIELDS, a, b) && sameFields(MEMBER_FIELDS, a.rev, b.rev);

const sameTrack = (a: Track, b: Track): boolean =>
  a.id === b.id && sameFields(TRACK_FIELDS, a, b) && sameFields(TRACK_FIELDS, a.rev, b.rev);

/** Same id, sample, values and revisions. */
export const sameClip = (a: Clip, b: Clip): boolean =>
  a.id === b.id &&
  sameSample(a.sample, b.sample) &&
  sameFields(CLIP_FIELDS, a, b) &&
  sameFields(CLIP_FIELDS, a.rev, b.rev);

const byId = (a: { readonly id: string }, b: { readonly id: string }): number =>
  a.id < b.id ? -1 : a.id > b.id ? 1 : 0;

function sameClips(a: readonly Clip[], b: readonly Clip[]): boolean {
  if (a.length !== b.length) return false;
  const sortedA = [...a].sort(byId);
  const sortedB = [...b].sort(byId);
  return sortedA.every((clip, i) => {
    const other = sortedB[i];
    return other !== undefined && sameClip(clip, other);
  });
}

// ---------------------------------------------------------------------------
// Checking whole objects and revisions, for inverses and snapshots
// ---------------------------------------------------------------------------

const isRevision = (value: unknown): value is OpId => typeof value === 'string' && value.length <= MAX_ID_LENGTH;

function isRevisionsOf<F extends string>(fields: readonly F[], value: unknown): value is Revisions<F> {
  return isObject(value) && onlyKeys(value, fields) && fields.every((field) => isRevision(value[field]));
}

function isFields<F extends string>(checks: Checks<F>, fields: readonly F[], value: Plain): boolean {
  return fields.every((field) => checks[field](value[field]));
}

function isNewClip(value: unknown): value is NewClip {
  return (
    isObject(value) &&
    isId(value['id']) &&
    isSampleRef(value['sample']) &&
    isFields(CLIP_FIELD_CHECKS, CLIP_FIELDS, value)
  );
}

const field = (value: unknown, key: string): unknown => (isObject(value) ? value[key] : undefined);

function isClip(value: unknown): value is Clip {
  return isNewClip(value) && isRevisionsOf(CLIP_FIELDS, field(value, 'rev'));
}

function isTrack(value: unknown): value is Track {
  return isNewTrack(value, isPosition) && isRevisionsOf(TRACK_FIELDS, field(value, 'rev'));
}

function isMember(value: unknown): value is Member {
  return (
    isNewMember(value) &&
    typeof field(value, 'removed') === 'boolean' &&
    isRevisionsOf(MEMBER_FIELDS, field(value, 'rev'))
  );
}

function isTimeline(value: unknown): value is Timeline {
  return (
    isObject(value) && isFields(TIMELINE_CHECKS, TIMELINE_FIELDS, value) && isRevisionsOf(TIMELINE_FIELDS, value['rev'])
  );
}

const isClipList = (value: unknown): value is readonly Clip[] => Array.isArray(value) && value.every(isClip);

/** Reads a `*.patch` change. Returns the fields it writes, or undefined if it is malformed. */
function readPatch<F extends string>(change: Plain, fields: readonly F[], checks: Checks<F>): F[] | undefined {
  const { set, expect, restore } = change;
  if (!isObject(set) || !isObject(expect) || !isObject(restore) || !onlyKeys(set, fields)) return undefined;
  const written = fields.filter((field) => has(set, field));
  if (written.length === 0) return undefined;
  if (!isRevisionsOf(written, expect) || !isRevisionsOf(written, restore)) return undefined;
  return written.every((field) => checks[field](set[field])) ? written : undefined;
}

function readSplitParts(change: Plain): (Plain & { right: Clip; left: Plain }) | undefined {
  const { clipId, right, left, expect, restore } = change;
  if (!isId(clipId) || !isClip(right) || right.id === clipId) return undefined;
  if (!isObject(left) || !onlyKeys(left, SPLIT_FIELDS) || !isLength(left['length']) || !isTick(left['fadeOut'])) {
    return undefined;
  }
  if (!isRevisionsOf(SPLIT_FIELDS, expect) || !isRevisionsOf(SPLIT_FIELDS, restore)) return undefined;
  return { ...change, right, left };
}

// ---------------------------------------------------------------------------
// Writing fields
// ---------------------------------------------------------------------------

interface Written<F extends string> {
  readonly values: Record<F, unknown>;
  readonly rev: Revisions<F>;
  /** The guarded patch that undoes this write. */
  readonly patch: {
    set: Partial<Record<F, unknown>>;
    expect: Partial<Record<F, OpId>>;
    restore: Partial<Record<F, OpId>>;
  };
}

/**
 * Write `target`'s values to `fields` of an object. Each written field gets the
 * revision `revFor` gives it. The patch puts back the old values, guarded on the
 * revisions this write leaves, and restores the revisions that were there.
 */
function writeFields<F extends string>(
  allFields: readonly F[],
  current: { readonly [K in F]: unknown } & { readonly rev: Revisions<F> },
  fields: readonly F[],
  target: Partial<Record<F, unknown>>,
  revFor: (field: F) => OpId,
): Written<F> {
  const values: Partial<Record<F, unknown>> = {};
  const rev: Partial<Record<F, OpId>> = {};
  const set: Partial<Record<F, unknown>> = {};
  const expect: Partial<Record<F, OpId>> = {};
  const restore: Partial<Record<F, OpId>> = {};
  for (const field of allFields) {
    const writes = fields.includes(field);
    values[field] = writes ? target[field] : current[field];
    rev[field] = writes ? revFor(field) : current.rev[field];
    if (writes) {
      set[field] = current[field];
      expect[field] = rev[field];
      restore[field] = current.rev[field];
    }
  }
  return {
    values: values as Record<F, unknown>,
    rev: rev as Revisions<F>,
    patch: { set, expect, restore },
  };
}

/** The fields in `candidates` whose target value differs from the current one. */
function changedFields<F extends string>(
  candidates: readonly F[],
  current: { readonly [K in F]: unknown },
  target: Partial<Record<F, unknown>>,
): F[] {
  return candidates.filter((field) => has(target, field) && !sameValue(current[field], target[field]));
}

const ok = (inverse: Change): ApplyResult => ({ applied: true, inverse });
const no = (reason: RejectReason): ApplyResult => ({ applied: false, reason });

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

export class Session implements SessionView {
  private timelineValue: Timeline;
  private readonly memberMap = new Map<MemberId, Member>();
  private readonly trackMap = new Map<TrackId, Track>();
  private readonly clipMap = new Map<ClipId, Clip>();
  /** For each track, the ids of the clips on it. Kept in step with `clipMap`. */
  private readonly clipIndex = new Map<TrackId, Set<ClipId>>();
  /** Sorted track ids, or null when a track was added, removed or moved since. */
  private orderCache: readonly TrackId[] | null = Object.freeze([]);

  private constructor() {
    this.timelineValue = makeTimeline(
      { tempo: DEFAULT_TEMPO, timeSignature: DEFAULT_TIME_SIGNATURE },
      revisions(TIMELINE_FIELDS, () => NO_REVISION),
    );
  }

  /** A new, empty session: 120 BPM in 4/4, no members, no tracks. */
  static empty(): Session {
    return new Session();
  }

  /**
   * A session holding exactly what a snapshot holds. Everything is checked as
   * an operation would be.
   *
   * @throws Error if the snapshot is malformed or breaks a structural rule.
   */
  static fromSnapshot(snapshot: SessionSnapshot): Session {
    const fail = (why: string): never => {
      throw new Error(`invalid session snapshot: ${why}`);
    };
    const data: unknown = snapshot;
    if (!isObject(data)) return fail('not an object');
    const { timeline, members, tracks, clips } = data;
    if (!isTimeline(timeline)) return fail('timeline');
    if (!Array.isArray(members) || !Array.isArray(tracks) || !Array.isArray(clips)) return fail('missing lists');

    const session = new Session();
    session.timelineValue = makeTimeline(timeline, timeline.rev);
    for (const member of members) {
      if (!isMember(member)) return fail('member');
      if (session.memberMap.has(member.id)) return fail(`member ${member.id} appears twice`);
      session.memberMap.set(member.id, makeMember(member.id, member, member.rev));
    }
    for (const track of tracks) {
      if (!isTrack(track)) return fail('track');
      if (session.trackMap.has(track.id)) return fail(`track ${track.id} appears twice`);
      session.putTrack(makeTrack(track.id, track, track.rev));
    }
    for (const clip of clips) {
      if (!isClip(clip)) return fail('clip');
      if (session.clipMap.has(clip.id)) return fail(`clip ${clip.id} appears twice`);
      if (!session.trackMap.has(clip.trackId)) return fail(`clip ${clip.id} is on missing track ${clip.trackId}`);
      session.putClip(makeClip(clip.id, clip.sample, clip, clip.rev));
    }
    return session;
  }

  /** An independent copy. Costs time in proportion to the session's size. */
  clone(): Session {
    return Session.fromSnapshot(this.snapshot());
  }

  get timeline(): Timeline {
    return this.timelineValue;
  }

  get members(): ReadonlyMap<MemberId, Member> {
    return this.memberMap;
  }

  get tracks(): ReadonlyMap<TrackId, Track> {
    return this.trackMap;
  }

  get clips(): ReadonlyMap<ClipId, Clip> {
    return this.clipMap;
  }

  get trackOrder(): readonly TrackId[] {
    if (this.orderCache === null) {
      const sorted = [...this.trackMap.values()].sort((a, b) =>
        a.position < b.position ? -1 : a.position > b.position ? 1 : byId(a, b),
      );
      this.orderCache = Object.freeze(sorted.map((track) => track.id));
    }
    return this.orderCache;
  }

  clipsOnTrack(trackId: TrackId): readonly Clip[] {
    return this.clipsOn(trackId).sort((a, b) => a.start - b.start || byId(a, b));
  }

  snapshot(): SessionSnapshot {
    return {
      timeline: this.timelineValue,
      members: [...this.memberMap.values()].sort(byId),
      tracks: this.trackOrder.map((id) => this.trackMap.get(id)).filter((track) => track !== undefined),
      clips: [...this.clipMap.values()].sort(byId),
    };
  }

  /**
   * Apply one operation: the only way the session changes.
   *
   * The operation may come straight off the network, so every part of it is
   * checked. Anything malformed, stale or conflicting leaves the session as it
   * was and is reported with a reason.
   */
  apply(operation: Operation): ApplyResult {
    const data: unknown = operation;
    if (!isObject(data) || !isId(data['id']) || !isId(data['author']) || !isObject(data['change'])) {
      return no('invalid');
    }
    return this.applyChange(data['change'], data['id']);
  }

  // -------------------------------------------------------------------------

  private applyChange(change: Plain, opId: OpId): ApplyResult {
    switch (change['type']) {
      case 'tempo.set':
        return isTempo(change['tempo']) ? this.editTimeline({ tempo: change['tempo'] }, opId) : no('invalid');
      case 'timeSignature.set':
        return isTimeSignature(change['timeSignature'])
          ? this.editTimeline({ timeSignature: change['timeSignature'] }, opId)
          : no('invalid');
      case 'timeline.patch':
        return this.patchTimeline(change);

      case 'member.add':
        return this.addMember(change, opId);
      case 'member.unadd':
        return this.unaddMember(change);
      case 'member.rename':
        return isMemberName(change['name'])
          ? this.editMember(change['memberId'], { name: change['name'] }, opId)
          : no('invalid');
      case 'member.recolour':
        return isColour(change['colour'])
          ? this.editMember(change['memberId'], { colour: change['colour'] }, opId)
          : no('invalid');
      case 'member.remove':
        return this.editMember(change['memberId'], { removed: true }, opId);
      case 'member.patch':
        return this.patchMember(change);

      case 'track.add':
        return this.addTrack(change, opId);
      case 'track.remove':
        return this.removeTrack(change);
      case 'track.rename':
        return isTrackName(change['name'])
          ? this.editTrack(change['trackId'], { name: change['name'] }, opId)
          : no('invalid');
      case 'track.move':
        return isPosition(change['position'])
          ? this.editTrack(change['trackId'], { position: change['position'] }, opId)
          : no('invalid');
      case 'track.patch':
        return this.patchTrack(change);

      case 'clip.add':
        return this.addClip(change, opId);
      case 'clip.remove':
        return this.removeClip(change);
      case 'clip.move': {
        const { trackId, start } = change;
        if (!isId(trackId) || !isTick(start)) return no('invalid');
        return this.editClip(change['clipId'], { trackId, start }, opId);
      }
      case 'clip.trim':
        return this.trimClip(change, opId);
      case 'clip.gain':
        return isGain(change['gain']) ? this.editClip(change['clipId'], { gain: change['gain'] }, opId) : no('invalid');
      case 'clip.fade': {
        const { fadeIn, fadeOut } = change;
        if (!isTick(fadeIn) || !isTick(fadeOut)) return no('invalid');
        return this.editClip(change['clipId'], { fadeIn, fadeOut }, opId);
      }
      case 'clip.split':
        return this.splitClip(change, opId);
      case 'clip.unsplit':
        return this.unsplitClip(change);
      case 'clip.resplit':
        return this.resplitClip(change);
      case 'clip.patch':
        return this.patchClip(change);

      default:
        return no('invalid');
    }
  }

  // ---- Timeline -----------------------------------------------------------

  private editTimeline(target: Partial<TimelineValues>, opId: OpId): ApplyResult {
    const fields = changedFields(TIMELINE_FIELDS, this.timelineValue, target);
    if (fields.length === 0) return no('no-change');
    return this.writeTimeline(fields, target, all(opId));
  }

  private patchTimeline(change: Plain): ApplyResult {
    const fields = readPatch(change, TIMELINE_FIELDS, TIMELINE_CHECKS);
    if (fields === undefined) return no('invalid');
    const { set, expect, restore } = change as unknown as FieldPatch<TimelineValues>;
    if (!fields.every((field) => this.timelineValue.rev[field] === expect[field])) return no('guard-failed');
    return this.writeTimeline(fields, set, (field) => restore[field] ?? NO_REVISION);
  }

  private writeTimeline(
    fields: readonly TimelineField[],
    target: Partial<TimelineValues>,
    revFor: (field: TimelineField) => OpId,
  ): ApplyResult {
    const written = writeFields(TIMELINE_FIELDS, this.timelineValue, fields, target, revFor);
    this.timelineValue = makeTimeline(written.values as unknown as TimelineValues, written.rev);
    return ok({ type: 'timeline.patch', ...(written.patch as FieldPatch<TimelineValues>) });
  }

  // ---- Members ------------------------------------------------------------

  private addMember(change: Plain, opId: OpId): ApplyResult {
    const { member, restore } = change;
    if (!isNewMember(member)) return no('invalid');
    if (
      restore !== undefined &&
      !(isObject(restore) && typeof restore['removed'] === 'boolean' && isRevisionsOf(MEMBER_FIELDS, restore['rev']))
    ) {
      return no('invalid');
    }
    if (this.memberMap.has(member.id)) return no('id-exists');
    const restored = restore as { removed: boolean; rev: Revisions<MemberField> } | undefined;
    const added = makeMember(
      member.id,
      { name: member.name, colour: member.colour, removed: restored?.removed ?? false },
      restored?.rev ?? revisions(MEMBER_FIELDS, all(opId)),
    );
    this.memberMap.set(added.id, added);
    return ok({ type: 'member.unadd', memberId: added.id, ifMember: added });
  }

  private unaddMember(change: Plain): ApplyResult {
    const { memberId, ifMember } = change;
    if (!isId(memberId) || !isMember(ifMember)) return no('invalid');
    const member = this.memberMap.get(memberId);
    if (member === undefined) return no('target-missing');
    if (!sameMember(member, ifMember)) return no('guard-failed');
    this.memberMap.delete(memberId);
    return ok({
      type: 'member.add',
      member: { id: member.id, name: member.name, colour: member.colour },
      restore: { removed: member.removed, rev: member.rev },
    });
  }

  private editMember(memberId: unknown, target: Partial<MemberValues>, opId: OpId): ApplyResult {
    if (!isId(memberId)) return no('invalid');
    const member = this.memberMap.get(memberId);
    if (member === undefined) return no('target-missing');
    const fields = changedFields(MEMBER_FIELDS, member, target);
    if (fields.length === 0) return no('no-change');
    return this.writeMember(member, fields, target, all(opId));
  }

  private patchMember(change: Plain): ApplyResult {
    const fields = readPatch(change, MEMBER_FIELDS, MEMBER_CHECKS);
    const memberId = change['memberId'];
    if (fields === undefined || !isId(memberId)) return no('invalid');
    const member = this.memberMap.get(memberId);
    if (member === undefined) return no('target-missing');
    const { set, expect, restore } = change as unknown as FieldPatch<MemberValues>;
    if (!fields.every((field) => member.rev[field] === expect[field])) return no('guard-failed');
    return this.writeMember(member, fields, set, (field) => restore[field] ?? NO_REVISION);
  }

  private writeMember(
    member: Member,
    fields: readonly MemberField[],
    target: Partial<MemberValues>,
    revFor: (field: MemberField) => OpId,
  ): ApplyResult {
    const written = writeFields(MEMBER_FIELDS, member, fields, target, revFor);
    this.memberMap.set(member.id, makeMember(member.id, written.values as unknown as MemberValues, written.rev));
    return ok({ type: 'member.patch', memberId: member.id, ...(written.patch as FieldPatch<MemberValues>) });
  }

  // ---- Tracks -------------------------------------------------------------

  private addTrack(change: Plain, opId: OpId): ApplyResult {
    const { track, restore } = change;
    if (!isNewTrack(track, isPosition)) return no('invalid');
    if (
      restore !== undefined &&
      !(isObject(restore) && isRevisionsOf(TRACK_FIELDS, restore['rev']) && isClipList(restore['clips']))
    ) {
      return no('invalid');
    }
    if (this.trackMap.has(track.id)) return no('id-exists');
    const restored = restore as { rev: Revisions<TrackField>; clips: readonly Clip[] } | undefined;
    const added = makeTrack(track.id, track, restored?.rev ?? revisions(TRACK_FIELDS, all(opId)));
    this.putTrack(added);

    // Put back the clips the track held, skipping any whose id has been taken since.
    const clips: Clip[] = [];
    for (const clip of [...(restored?.clips ?? [])].sort(byId)) {
      if (clip.trackId !== added.id || this.clipMap.has(clip.id)) continue;
      const copy = makeClip(clip.id, clip.sample, clip, clip.rev);
      this.putClip(copy);
      clips.push(copy);
    }
    return ok({ type: 'track.remove', trackId: added.id, ifUnchanged: { track: added, clips } });
  }

  private removeTrack(change: Plain): ApplyResult {
    const { trackId, ifUnchanged } = change;
    if (!isId(trackId)) return no('invalid');
    if (
      ifUnchanged !== undefined &&
      !(isObject(ifUnchanged) && isTrack(ifUnchanged['track']) && isClipList(ifUnchanged['clips']))
    ) {
      return no('invalid');
    }
    const track = this.trackMap.get(trackId);
    if (track === undefined) return no('target-missing');
    const onTrack = this.clipsOn(trackId).sort(byId);
    const guard = ifUnchanged as { track: Track; clips: readonly Clip[] } | undefined;
    if (guard !== undefined && !(sameTrack(track, guard.track) && sameClips(onTrack, guard.clips))) {
      return no('guard-failed');
    }
    for (const clip of onTrack) this.deleteClip(clip);
    this.deleteTrack(track);
    return ok({
      type: 'track.add',
      track: { id: track.id, name: track.name, position: track.position },
      restore: { rev: track.rev, clips: onTrack },
    });
  }

  private editTrack(trackId: unknown, target: Partial<TrackValues>, opId: OpId): ApplyResult {
    if (!isId(trackId)) return no('invalid');
    const track = this.trackMap.get(trackId);
    if (track === undefined) return no('target-missing');
    const fields = changedFields(TRACK_FIELDS, track, target);
    if (fields.length === 0) return no('no-change');
    return this.writeTrack(track, fields, target, all(opId));
  }

  private patchTrack(change: Plain): ApplyResult {
    const fields = readPatch(change, TRACK_FIELDS, TRACK_CHECKS);
    const trackId = change['trackId'];
    if (fields === undefined || !isId(trackId)) return no('invalid');
    const track = this.trackMap.get(trackId);
    if (track === undefined) return no('target-missing');
    const { set, expect, restore } = change as unknown as FieldPatch<TrackValues>;
    if (!fields.every((field) => track.rev[field] === expect[field])) return no('guard-failed');
    return this.writeTrack(track, fields, set, (field) => restore[field] ?? NO_REVISION);
  }

  private writeTrack(
    track: Track,
    fields: readonly TrackField[],
    target: Partial<TrackValues>,
    revFor: (field: TrackField) => OpId,
  ): ApplyResult {
    const written = writeFields(TRACK_FIELDS, track, fields, target, revFor);
    this.putTrack(makeTrack(track.id, written.values as unknown as TrackValues, written.rev));
    return ok({ type: 'track.patch', trackId: track.id, ...(written.patch as FieldPatch<TrackValues>) });
  }

  // ---- Clips --------------------------------------------------------------

  private addClip(change: Plain, opId: OpId): ApplyResult {
    const { clip, rev } = change;
    if (!isNewClip(clip)) return no('invalid');
    if (rev !== undefined && !isRevisionsOf(CLIP_FIELDS, rev)) return no('invalid');
    if (this.clipMap.has(clip.id)) return no('id-exists');
    if (!this.trackMap.has(clip.trackId)) return no('track-missing');
    const added = makeClip(
      clip.id,
      clip.sample,
      clip,
      (rev as Revisions<ClipField> | undefined) ?? revisions(CLIP_FIELDS, all(opId)),
    );
    this.putClip(added);
    return ok({ type: 'clip.remove', clipId: added.id, ifClip: added });
  }

  private removeClip(change: Plain): ApplyResult {
    const { clipId, ifClip } = change;
    if (!isId(clipId) || (ifClip !== undefined && !isClip(ifClip))) return no('invalid');
    const clip = this.clipMap.get(clipId);
    if (clip === undefined) return no('target-missing');
    if (ifClip !== undefined && !sameClip(clip, ifClip as Clip)) return no('guard-failed');
    this.deleteClip(clip);
    return ok({ type: 'clip.add', clip: newClipOf(clip), rev: clip.rev });
  }

  /**
   * Move one edge or both. An edge not given stays where it is, and the audio
   * does not move: the offset follows the left edge, at the session's tempo.
   */
  private trimClip(change: Plain, opId: OpId): ApplyResult {
    const { clipId, start, end } = change;
    if (!isId(clipId) || (start === undefined && end === undefined)) return no('invalid');
    if ((start !== undefined && !isTick(start)) || (end !== undefined && !isLength(end))) return no('invalid');
    const clip = this.clipMap.get(clipId);
    if (clip === undefined) return no('target-missing');
    const newStart = start === undefined ? clip.start : (start as number);
    const newEnd = end === undefined ? clip.start + clip.length : (end as number);
    if (newEnd <= newStart) return no('out-of-range');
    const shifted = clip.offset + ticksToSeconds(newStart - clip.start, this.timelineValue.tempo);
    // Trimming back to the very start of the file can leave a rounding error either side of zero.
    const offset = Math.abs(shifted) < OFFSET_ROUNDING ? 0 : shifted;
    // The left edge cannot go further left than the start of the file.
    if (offset < 0) return no('out-of-range');
    const target: Partial<ClipValues> = { start: newStart, length: newEnd - newStart, offset };
    const fields = changedFields(CLIP_FIELDS, clip, target);
    if (fields.length === 0) return no('no-change');
    return this.writeClip(clip, fields, target, all(opId));
  }

  /** A direct edit: write the fields whose value actually changes, stamped with this operation. */
  private editClip(clipId: unknown, target: Partial<ClipValues>, opId: OpId): ApplyResult {
    if (!isId(clipId)) return no('invalid');
    const clip = this.clipMap.get(clipId);
    if (clip === undefined) return no('target-missing');
    if (target.trackId !== undefined && !this.trackMap.has(target.trackId)) return no('track-missing');
    const fields = changedFields(CLIP_FIELDS, clip, target);
    if (fields.length === 0) return no('no-change');
    return this.writeClip(clip, fields, target, all(opId));
  }

  private patchClip(change: Plain): ApplyResult {
    const fields = readPatch(change, CLIP_FIELDS, CLIP_FIELD_CHECKS);
    const clipId = change['clipId'];
    if (fields === undefined || !isId(clipId)) return no('invalid');
    const clip = this.clipMap.get(clipId);
    if (clip === undefined) return no('target-missing');
    const { set, expect, restore } = change as unknown as FieldPatch<ClipValues>;
    if (!fields.every((field) => clip.rev[field] === expect[field])) return no('guard-failed');
    if (set.trackId !== undefined && !this.trackMap.has(set.trackId)) return no('track-missing');
    return this.writeClip(clip, fields, set, (field) => restore[field] ?? NO_REVISION);
  }

  private writeClip(
    clip: Clip,
    fields: readonly ClipField[],
    target: Partial<ClipValues>,
    revFor: (field: ClipField) => OpId,
  ): ApplyResult {
    const written = writeFields(CLIP_FIELDS, clip, fields, target, revFor);
    this.putClip(makeClip(clip.id, clip.sample, written.values as unknown as ClipValues, written.rev), clip);
    return ok({ type: 'clip.patch', clipId: clip.id, ...(written.patch as FieldPatch<ClipValues>) });
  }

  private splitClip(change: Plain, opId: OpId): ApplyResult {
    const { clipId, newClipId, at } = change;
    if (!isId(clipId) || !isId(newClipId) || clipId === newClipId || !isLength(at)) return no('invalid');
    const left = this.clipMap.get(clipId);
    if (left === undefined) return no('target-missing');
    if (this.clipMap.has(newClipId)) return no('id-exists');
    if (at >= left.length || !isTick(left.start + at)) return no('out-of-range');

    const right = makeClip(
      newClipId,
      left.sample,
      {
        trackId: left.trackId,
        start: left.start + at,
        length: left.length - at,
        offset: left.offset + ticksToSeconds(at, this.timelineValue.tempo),
        gain: left.gain,
        fadeIn: 0,
        fadeOut: left.fadeOut,
      },
      revisions(CLIP_FIELDS, all(opId)),
    );
    const written = writeFields(CLIP_FIELDS, left, SPLIT_FIELDS, { length: at, fadeOut: 0 }, all(opId));
    this.putClip(makeClip(left.id, left.sample, written.values as unknown as ClipValues, written.rev), left);
    this.putClip(right);
    return ok({
      type: 'clip.unsplit',
      clipId: left.id,
      right,
      left: { length: left.length, fadeOut: left.fadeOut },
      expect: { length: opId, fadeOut: opId },
      restore: { length: left.rev.length, fadeOut: left.rev.fadeOut },
    });
  }

  /** Shared by unsplit and resplit, which differ only in whether the right part goes or comes back. */
  private splitParts(change: Plain, rightPresent: boolean): ApplyResult {
    const parts = readSplitParts(change);
    if (parts === undefined) return no('invalid');
    const { right } = parts;
    const left = this.clipMap.get(parts['clipId'] as ClipId);
    if (left === undefined) return no('target-missing');
    const expect = parts['expect'] as Revisions<'length' | 'fadeOut'>;
    const restore = parts['restore'] as Revisions<'length' | 'fadeOut'>;
    const values = parts.left as { length: number; fadeOut: number };

    const current = this.clipMap.get(right.id);
    if (rightPresent) {
      if (current === undefined) return no('target-missing');
    } else if (current !== undefined) {
      return no('id-exists');
    }
    if (left.rev.length !== expect.length || left.rev.fadeOut !== expect.fadeOut) return no('guard-failed');
    if (rightPresent && !sameClip(current as Clip, right)) return no('guard-failed');
    if (!rightPresent && !this.trackMap.has(right.trackId)) return no('track-missing');

    const written = writeFields(CLIP_FIELDS, left, SPLIT_FIELDS, values, (field) =>
      field === 'length' ? restore.length : restore.fadeOut,
    );
    this.putClip(makeClip(left.id, left.sample, written.values as unknown as ClipValues, written.rev), left);
    let rightNow: Clip;
    if (rightPresent) {
      rightNow = current as Clip;
      this.deleteClip(rightNow);
    } else {
      rightNow = makeClip(right.id, right.sample, right, right.rev);
      this.putClip(rightNow);
    }
    return ok({
      type: rightPresent ? 'clip.resplit' : 'clip.unsplit',
      clipId: left.id,
      right: rightNow,
      left: { length: left.length, fadeOut: left.fadeOut },
      expect: { length: restore.length, fadeOut: restore.fadeOut },
      restore: { length: left.rev.length, fadeOut: left.rev.fadeOut },
    });
  }

  private unsplitClip(change: Plain): ApplyResult {
    return this.splitParts(change, true);
  }

  private resplitClip(change: Plain): ApplyResult {
    return this.splitParts(change, false);
  }

  // ---- Storage, keeping the indexes in step ------------------------------

  private clipsOn(trackId: TrackId): Clip[] {
    const ids = this.clipIndex.get(trackId);
    const clips: Clip[] = [];
    if (ids === undefined) return clips;
    for (const id of ids) {
      const clip = this.clipMap.get(id);
      if (clip !== undefined) clips.push(clip);
    }
    return clips;
  }

  private putTrack(track: Track): void {
    const previous = this.trackMap.get(track.id);
    this.trackMap.set(track.id, track);
    if (!this.clipIndex.has(track.id)) this.clipIndex.set(track.id, new Set());
    if (previous === undefined || previous.position !== track.position) this.orderCache = null;
  }

  private deleteTrack(track: Track): void {
    this.trackMap.delete(track.id);
    this.clipIndex.delete(track.id);
    this.orderCache = null;
  }

  /** Store a new or changed clip. `previous` is the version it replaces, if any. */
  private putClip(clip: Clip, previous?: Clip): void {
    if (previous !== undefined && previous.trackId !== clip.trackId) {
      this.clipIndex.get(previous.trackId)?.delete(clip.id);
    }
    this.clipMap.set(clip.id, clip);
    this.clipIndex.get(clip.trackId)?.add(clip.id);
  }

  private deleteClip(clip: Clip): void {
    this.clipMap.delete(clip.id);
    this.clipIndex.get(clip.trackId)?.delete(clip.id);
  }

  checkInvariants(): string[] {
    const problems: string[] = [];
    if (!isTimeline(this.timelineValue)) problems.push('timeline has an invalid value');
    for (const [id, member] of this.memberMap) {
      if (member.id !== id || !isMember(member)) problems.push(`member ${id} is invalid`);
    }
    for (const [id, track] of this.trackMap) {
      if (track.id !== id || !isTrack(track)) problems.push(`track ${id} is invalid`);
      if (!this.clipIndex.has(id)) problems.push(`track ${id} has no clip index`);
    }
    for (const [id, clip] of this.clipMap) {
      if (clip.id !== id || !isClip(clip)) problems.push(`clip ${id} is invalid`);
      if (!this.trackMap.has(clip.trackId)) problems.push(`clip ${id} is on missing track ${clip.trackId}`);
      if (!this.clipIndex.get(clip.trackId)?.has(id)) problems.push(`clip ${id} is missing from its track's index`);
    }
    let indexed = 0;
    for (const [trackId, ids] of this.clipIndex) {
      if (!this.trackMap.has(trackId)) problems.push(`clip index holds missing track ${trackId}`);
      for (const id of ids) {
        indexed += 1;
        if (this.clipMap.get(id)?.trackId !== trackId) problems.push(`clip index puts ${id} on the wrong track`);
      }
    }
    if (indexed !== this.clipMap.size) problems.push('clip index and clips disagree');
    const order = this.trackOrder;
    if (order.length !== this.trackMap.size || new Set(order).size !== order.length) {
      problems.push('track order and tracks disagree');
    }
    return problems;
  }
}

/**
 * A stable text form of a session, revisions included. Two sessions are equal
 * exactly when their canonical forms are, whatever order things were added in.
 */
export function canonical(session: SessionView): string {
  return stableStringify(session.snapshot());
}

function stableStringify(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stableStringify).join(',')}]`;
  if (isObject(value)) {
    const keys = Object.keys(value)
      .filter((key) => value[key] !== undefined)
      .sort();
    return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(value[key])}`).join(',')}}`;
  }
  return JSON.stringify(value);
}
