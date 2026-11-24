// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The shapes of the session and of the operations that change it.
 *
 * Every editable field of every object has a revision: the id of the operation
 * that last wrote it. Revisions are what make an inverse safe to apply later:
 * an inverse only writes a field while that field still holds the revision the
 * original operation left (ADR-0004, `spikes/session-model` finding 2).
 */

/** Identifies one operation. Opaque, and unique across the whole session. */
export type OpId = string;

/** A member's id. Opaque, made by the app, never by the relay (ADR-0006). */
export type MemberId = string;

export type TrackId = string;
export type ClipId = string;

/** The revision of a field no operation has written yet, such as the default tempo. */
export const NO_REVISION: OpId = '';

/** For each field in `F`, the operation that last wrote it. */
export type Revisions<F extends string> = { readonly [K in F]: OpId };

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

export interface TimeSignature {
  readonly numerator: number;
  readonly denominator: number;
}

/** One tempo and one time signature per project (DAW-5). */
export interface TimelineValues {
  /** Quarter notes per minute, whatever the time signature. */
  readonly tempo: number;
  readonly timeSignature: TimeSignature;
}

export type TimelineField = keyof TimelineValues;

export interface Timeline extends TimelineValues {
  readonly rev: Revisions<TimelineField>;
}

/** A person in the project (ADR-0006). Members are marked as removed, never deleted. */
export interface NewMember {
  readonly id: MemberId;
  readonly name: string;
  /** `#rrggbb`, lowercase. */
  readonly colour: string;
}

export interface MemberValues {
  readonly name: string;
  readonly colour: string;
  readonly removed: boolean;
}

export type MemberField = keyof MemberValues;

export interface Member extends MemberValues {
  readonly id: MemberId;
  readonly rev: Revisions<MemberField>;
}

export interface TrackValues {
  readonly name: string;
  /**
   * Where the track sits in the track list. Tracks are ordered by this key, then
   * by id. See `position.ts`: a reorder writes one track's key and nothing else,
   * so it does not disturb a track someone else adds at the same time.
   */
  readonly position: string;
}

export type TrackField = keyof TrackValues;

export interface NewTrack extends TrackValues {
  readonly id: TrackId;
}

export interface Track extends NewTrack {
  readonly rev: Revisions<TrackField>;
}

/**
 * The audio a clip plays, by content (ADR-0008). Never a path: where the file
 * lives on a machine is that machine's business and stays on it.
 */
export interface SampleRef {
  /** SHA-256 of the file's bytes, 64 lowercase hex digits. The sample's identity. */
  readonly fileHash: string;
  /** SHA-256 of the decoded PCM audio, when the indexer could compute it. */
  readonly audioHash?: string;
  /** The file's name when the clip was made, for display and for suggestions. */
  readonly name: string;
}

export interface ClipValues {
  readonly trackId: TrackId;
  /** Position on the timeline, in ticks. */
  readonly start: number;
  /** Duration in ticks. Always more than zero. */
  readonly length: number;
  /**
   * Seconds into the file where the clip's audio starts. Seconds of the file,
   * not ticks, so a tempo change does not change which audio a clip starts on.
   */
  readonly offset: number;
  /** Clip gain in decibels. */
  readonly gain: number;
  /** Fade lengths in ticks. They may add up to more than the clip; see `effectiveFades`. */
  readonly fadeIn: number;
  readonly fadeOut: number;
}

export type ClipField = keyof ClipValues;

export interface NewClip extends ClipValues {
  readonly id: ClipId;
  /** Fixed for the life of the clip. A different sample is a different clip. */
  readonly sample: SampleRef;
}

export interface Clip extends NewClip {
  readonly rev: Revisions<ClipField>;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

/**
 * A guarded write to some fields of one object. Only ever produced as an inverse.
 *
 * It applies only while every field in `set` was last written by the operation
 * named in `expect`, and when it applies each field gets back the revision in
 * `restore`. Both must name exactly the fields in `set`.
 */
export interface FieldPatch<V> {
  readonly set: Partial<V>;
  readonly expect: Partial<{ readonly [K in keyof V]: OpId }>;
  readonly restore: Partial<{ readonly [K in keyof V]: OpId }>;
}

/**
 * The two halves of a split, as its inverses carry them. `clip.unsplit` takes
 * the right part away and `clip.resplit` puts it back; each is the other's
 * inverse, and neither recomputes anything, so each undoes the other exactly.
 */
export interface SplitParts {
  /** The left part, which keeps the original clip's id. */
  readonly clipId: ClipId;
  /** The right part, exactly. */
  readonly right: Clip;
  /** The left part's length and fade-out to write. */
  readonly left: { readonly length: number; readonly fadeOut: number };
  /** Applies only while the left part's length and fade-out were last written by these operations. */
  readonly expect: Revisions<'length' | 'fadeOut'>;
  /** The revisions they get back. */
  readonly restore: Revisions<'length' | 'fadeOut'>;
}

/**
 * Every way the session can change. An operation carries one of these.
 *
 * Direct edits (add, remove, rename, move, trim, gain, fade and so on) are what
 * a person does, and they are unconditional: of two edits to the same field, the
 * later one in the shared order wins. The forms with `restore`, `rev`,
 * `ifUnchanged` or `ifClip`, and the `*.patch`, `clip.unsplit`, `clip.resplit`
 * and `member.unadd` types, are inverses. They are guarded by revisions, so an
 * inverse applied late never overwrites something another person changed in the
 * meantime.
 */
export type Change =
  // Timeline
  | { readonly type: 'tempo.set'; readonly tempo: number }
  | { readonly type: 'timeSignature.set'; readonly timeSignature: TimeSignature }
  | ({ readonly type: 'timeline.patch' } & FieldPatch<TimelineValues>)
  // Members
  | {
      readonly type: 'member.add';
      readonly member: NewMember;
      /** Present when this puts back a member that `member.unadd` took out. */
      readonly restore?: { readonly removed: boolean; readonly rev: Revisions<MemberField> };
    }
  | { readonly type: 'member.rename'; readonly memberId: MemberId; readonly name: string }
  | { readonly type: 'member.recolour'; readonly memberId: MemberId; readonly colour: string }
  /** Marks the member as removed. Their old edits keep naming them. */
  | { readonly type: 'member.remove'; readonly memberId: MemberId }
  | ({ readonly type: 'member.patch'; readonly memberId: MemberId } & FieldPatch<MemberValues>)
  /**
   * The inverse of `member.add`, so an unconfirmed add can be rolled back. Applies
   * only while the member is exactly as added. Not something a person does: members
   * are never deleted (ADR-0006).
   */
  | { readonly type: 'member.unadd'; readonly memberId: MemberId; readonly ifMember: Member }
  // Tracks
  | {
      readonly type: 'track.add';
      readonly track: NewTrack;
      /** Present when this puts back a removed track: its revisions and the clips it held. */
      readonly restore?: { readonly rev: Revisions<TrackField>; readonly clips: readonly Clip[] };
    }
  | {
      readonly type: 'track.remove';
      readonly trackId: TrackId;
      /** Present on an inverse: the track and its clips must be exactly these. */
      readonly ifUnchanged?: { readonly track: Track; readonly clips: readonly Clip[] };
    }
  | { readonly type: 'track.rename'; readonly trackId: TrackId; readonly name: string }
  /** Reorder: give the track a new position key. See `trackPositionAt`. */
  | { readonly type: 'track.move'; readonly trackId: TrackId; readonly position: string }
  | ({ readonly type: 'track.patch'; readonly trackId: TrackId } & FieldPatch<TrackValues>)
  // Clips
  | {
      readonly type: 'clip.add';
      readonly clip: NewClip;
      /** Present when this puts back a removed clip. */
      readonly rev?: Revisions<ClipField>;
    }
  | {
      readonly type: 'clip.remove';
      readonly clipId: ClipId;
      /** Present on an inverse: the clip must be exactly this. */
      readonly ifClip?: Clip;
    }
  /** Move to another place, on the same track or another one. */
  | { readonly type: 'clip.move'; readonly clipId: ClipId; readonly trackId: TrackId; readonly start: number }
  /**
   * Move one edge of a clip, or both, to a tick on the timeline. Give `start`
   * for the left edge, `end` for the right edge, or both.
   *
   * A trim never moves the audio. Each edge given moves on its own and the other
   * stays put, so two people trimming the two edges of one clip at once both
   * keep their trim. The model works out the new `offset` itself from how far
   * the left edge moved, at the session's tempo, so the audio that remains
   * stays exactly where it was, even if someone changed the tempo just before.
   */
  | {
      readonly type: 'clip.trim';
      readonly clipId: ClipId;
      readonly start?: number;
      readonly end?: number;
    }
  /**
   * Cut a clip in two, `at` ticks from its start. The clip keeps the left part
   * and its fade-in; the right part becomes `newClipId` and takes the fade-out.
   * `at` counts from the clip's start, so a split still cuts the same audio if
   * someone moved the clip in the meantime.
   */
  | { readonly type: 'clip.split'; readonly clipId: ClipId; readonly newClipId: ClipId; readonly at: number }
  /** The inverse of a split: remove the right part, which must be exactly `right`, and give the left part back its length. */
  | ({ readonly type: 'clip.unsplit' } & SplitParts)
  /** The inverse of an unsplit: put the right part back exactly and shorten the left part again. */
  | ({ readonly type: 'clip.resplit' } & SplitParts)
  | { readonly type: 'clip.gain'; readonly clipId: ClipId; readonly gain: number }
  | { readonly type: 'clip.fade'; readonly clipId: ClipId; readonly fadeIn: number; readonly fadeOut: number }
  | ({ readonly type: 'clip.patch'; readonly clipId: ClipId } & FieldPatch<ClipValues>);

export type ChangeType = Change['type'];

/** One edit, as it goes into the shared order. */
export interface Operation {
  /** Lets a resent operation be recognised. Unique across the session. */
  readonly id: OpId;
  /** The member who made it. The relay stamps this from the connection it has checked (ADR-0006). */
  readonly author: MemberId;
  readonly change: Change;
}

/** Why an operation had no effect. None of these is an error: the state is unchanged and valid. */
export type RejectReason =
  /** Malformed: a wrong type, a value out of range, a path where a name belongs. */
  | 'invalid'
  /** Adds something whose id is already taken. */
  | 'id-exists'
  /** The track, clip or member it edits is gone. */
  | 'target-missing'
  /** The track a clip would go to is gone. */
  | 'track-missing'
  /** The value no longer fits the target, such as a split point past a clip someone shortened. */
  | 'out-of-range'
  /** An inverse whose target someone else has written since. */
  | 'guard-failed'
  /** The state already is what the operation asks for. */
  | 'no-change';

export type ApplyResult =
  | {
      readonly applied: true;
      /** The guarded change that takes the session back to exactly where it was. */
      readonly inverse: Change;
    }
  | { readonly applied: false; readonly reason: RejectReason };
