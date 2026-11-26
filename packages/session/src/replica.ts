// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * One person's copy of the session, kept in step with the shared order
 * (ADR-0004), and that person's undo and redo history (SYN-5).
 *
 * A replica holds one mutable state, its view: every operation the relay has
 * ordered so far, plus this person's own operations that it has not ordered
 * yet. Local edits apply at once. When someone else's operation arrives, the
 * replica rolls its unconfirmed operations back with their inverses, applies
 * the new one, and applies its own again on top. That costs time in proportion
 * to the number of unconfirmed operations, whatever the size of the session.
 * When its own operation comes back from the relay in order, nothing needs
 * rolling back at all.
 *
 * Undo sends the inverse of this member's latest edit still in effect as a new
 * operation. Inverses are guarded by revisions, so an undo only ever changes
 * what this member's edit last wrote. An inverse is only right against the
 * shared order, so an undo of an edit the relay has not confirmed yet is shown
 * at once but held back, with anything made after it, until that edit is
 * confirmed; it then goes out carrying the inverse from the shared order.
 */

import { Session, type SessionSnapshot, type SessionView } from './session';
import type { Change, MemberId, OpId, Operation, RejectReason } from './types';

/** An operation after the relay, or a `Sequencer` when working solo, has given it its place in the order. */
export interface Sequenced {
  /** 1 for the session's first operation, then 2, 3 and so on, with no gaps. */
  readonly seq: number;
  readonly operation: Operation;
}

/** One edit in a member's undo or redo history. */
export interface HistoryEntry {
  /** The operation this entry reverses: an edit on the undo side, an undo on the redo side. */
  readonly opId: OpId;
  /** The guarded change that reverses it, as computed in the shared order. */
  readonly inverse: Change;
}

/**
 * One member's undo and redo history as plain data, so the app can hand it to a
 * new replica for the same member (ADR-0006). Each side is oldest first: the
 * next undo tries the last entry of `undo`.
 */
export interface UndoHistory {
  readonly member: MemberId;
  readonly undo: readonly HistoryEntry[];
  readonly redo: readonly HistoryEntry[];
}

export interface ReplicaOptions {
  /** The member this replica edits as. Every operation it makes carries this id. */
  readonly author: MemberId;
  /**
   * Makes the id of each new operation. Ids must never repeat within a session,
   * across restarts of the app too, or a resent operation could not be told
   * from a new one. See `opIdSource`.
   */
  readonly nextOpId: () => OpId;
  /** The confirmed state to start from. Default: an empty session. The replica takes it over. */
  readonly session?: Session;
  /** The sequence number of the last operation `session` includes. Default 0. */
  readonly seq?: number;
  /** The member's undo and redo history, from an earlier replica's `history`. Default: none. */
  readonly history?: UndoHistory;
}

export type SubmitResult =
  | {
      readonly applied: true;
      readonly operation: Operation;
      /**
       * The inverse against the view as it is now. If another person's operation
       * is ordered first, the true inverse can change; the replica keeps track.
       */
      readonly inverse: Change;
      /**
       * Send these to the relay, in this order. Usually just `operation`; empty
       * while an earlier undo waits for the relay (see `undo`), in which case a
       * later `receive` hands it over.
       */
      readonly send: readonly Operation[];
    }
  | { readonly applied: false; readonly reason: RejectReason };

export type UndoResult =
  | {
      readonly applied: true;
      readonly operation: Operation;
      /** The operation this reverses: the edit undone, or the undo redone. */
      readonly reverses: OpId;
      /**
       * Newer confirmed entries passed over and forgotten because they were no
       * longer in effect: someone else changed or removed what they changed.
       * Unconfirmed edits with no effect in the view for now are passed over too,
       * but kept, and are not listed here.
       */
      readonly skipped: readonly OpId[];
      /** Send these to the relay, in this order, as with `submit`. */
      readonly send: readonly Operation[];
    }
  | {
      /**
       * Nothing on this side of the history is in effect now. All that is left
       * on it, if anything, is unconfirmed edits that may yet take effect.
       */
      readonly applied: false;
      readonly skipped: readonly OpId[];
    };

export type ReceiveResult = (
  | /** Someone else's operation. `applied` says whether it changed anything. */
  { readonly kind: 'remote'; readonly applied: boolean }
  /** One of this replica's own operations, confirmed, and it took effect. */
  | { readonly kind: 'confirmed'; readonly operation: Operation }
  /**
   * One of this replica's own operations, confirmed, but it had no effect,
   * usually because someone else removed its target first. It is already gone
   * from the view and from the undo history. The interface should tell the person.
   */
  | { readonly kind: 'rejected'; readonly operation: Operation; readonly reason: RejectReason }
) & {
  /** Own operations this message released, to send to the relay in this order. Usually none. */
  readonly send: readonly Operation[];
};

/** A history entry. While its operation is unconfirmed, its inverse follows rollbacks and may be null. */
interface Entry {
  readonly opId: OpId;
  inverse: Change | null;
}

interface Pending {
  /** Changes only while `reverses` is set: it then carries that operation's latest inverse. */
  operation: Operation;
  /** The inverse against the view, or null when the operation has no effect there. */
  inverse: Change | null;
  /** Why it has no effect there, when it has none. */
  reason: RejectReason | null;
  /** Its entry in the undo or redo history, if it has one, kept in step with `inverse`. */
  readonly entry: Entry | null;
  /**
   * For an undo or redo of an operation that is itself unconfirmed: that
   * operation. Until the relay confirms it, this one is held back.
   */
  reverses: Pending | null;
  /** Handed over for sending. Own operations are sent in the order they were made. */
  sent: boolean;
}

export class Replica {
  readonly author: MemberId;
  private readonly nextOpId: () => OpId;
  private readonly state: Session;
  private confirmedSeq: number;
  private readonly pending: Pending[] = [];
  /** This member's edits that undo can reverse, oldest first. */
  private readonly undoStack: Entry[] = [];
  /** This member's undos that redo can reverse, oldest first. */
  private readonly redoStack: Entry[] = [];
  /** Why the replica can no longer be used, once a rollback has failed. */
  private brokenBy: string | null = null;

  /** @throws Error if `history` is another member's. */
  constructor(options: ReplicaOptions) {
    this.author = options.author;
    this.nextOpId = options.nextOpId;
    this.state = options.session ?? Session.empty();
    this.confirmedSeq = options.seq ?? 0;
    const { history } = options;
    if (history !== undefined) {
      if (history.member !== this.author) {
        throw new Error(`the undo history is member ${history.member}'s, not ${this.author}'s`);
      }
      // A damaged entry does no harm: its inverse is checked like any operation and skipped if it does not apply.
      for (const { opId, inverse } of history.undo) this.undoStack.push({ opId, inverse });
      for (const { opId, inverse } of history.redo) this.redoStack.push({ opId, inverse });
    }
  }

  /** What the interface draws: the confirmed state plus this person's unconfirmed operations. */
  get view(): SessionView {
    return this.state;
  }

  /** The sequence number of the last operation received from the relay. */
  get seq(): number {
    return this.confirmedSeq;
  }

  /**
   * Own operations not yet ordered by the relay, oldest first, whether sent or
   * held back. Folding them onto the confirmed state gives the view.
   */
  get unconfirmed(): readonly Operation[] {
    // A held undo whose edit has no effect in the view is not applied either.
    return this.pending.filter((entry) => entry.inverse !== null || entry.reverses === null).map((e) => e.operation);
  }

  /**
   * Make an edit. It applies to the view at once. If it applied, send what
   * `send` holds to the relay; if not, nothing changed and there is nothing to
   * send. It clears the redo side of the history.
   */
  submit(change: Change): SubmitResult {
    this.checkUsable();
    // Joining is not something a person undoes: members are never deleted (ADR-0006, ADR-0014).
    const undoable = change.type !== 'member.add';
    const made = this.make(change, null, undoable);
    if (!made.applied) return made;
    if (made.pending.entry !== null) {
      this.undoStack.push(made.pending.entry);
      this.redoStack.length = 0;
    }
    return { applied: true, operation: made.pending.operation, inverse: made.inverse, send: this.release() };
  }

  /**
   * Undo this member's most recent edit that is still in effect (SYN-5).
   *
   * The undo is the edit's inverse, sent as a new operation that the relay
   * orders like any other. It applies only while every field it writes was last
   * written by the edit it reverses, and it puts back the revisions that were
   * there before, so the undo before it is in effect again. Edits whose fields
   * someone else has written since, or whose target is gone, are skipped and
   * forgotten, and the next older edit is tried. An unconfirmed edit with no
   * effect in the view is passed over but kept: only the relay's order says
   * whether it has an effect, and if it has none it comes back `rejected` and
   * leaves the history then.
   *
   * If someone else's edit to the same thing is ordered just before the undo,
   * the undo comes back `rejected`, like any edit of this member's that had no
   * effect, and their edit stands. If theirs is ordered just after, it
   * overwrites the undo as any later edit would.
   */
  undo(): UndoResult {
    return this.reverse(this.undoStack, this.redoStack);
  }

  /** Redo this member's most recent undo still in effect. Works as `undo` does. */
  redo(): UndoResult {
    return this.reverse(this.redoStack, this.undoStack);
  }

  /** True when the undo side holds anything. Undo may still find none of it in effect. */
  get canUndo(): boolean {
    return this.undoStack.length > 0;
  }

  /** True when the redo side holds anything. Redo may still find none of it in effect. */
  get canRedo(): boolean {
    return this.redoStack.length > 0;
  }

  /**
   * This member's undo and redo history, to hand to a new replica for the same
   * member. Confirmed entries only: an unconfirmed operation's inverse depends
   * on where the relay puts it, and a new replica starts from confirmed state.
   */
  get history(): UndoHistory {
    const unconfirmed = new Set(this.pending.map((entry) => entry.operation.id));
    const confirmed = (stack: readonly Entry[]): readonly HistoryEntry[] =>
      Object.freeze(
        stack.flatMap(({ opId, inverse }) =>
          inverse === null || unconfirmed.has(opId) ? [] : [Object.freeze({ opId, inverse })],
        ),
      );
    return Object.freeze({ member: this.author, undo: confirmed(this.undoStack), redo: confirmed(this.redoStack) });
  }

  /**
   * True once a rollback has failed. The view may then be half rolled back, so
   * `submit` and `receive` refuse to go on, and the app must build a new replica.
   */
  get broken(): boolean {
    return this.brokenBy !== null;
  }

  /**
   * Take the next operation in the shared order.
   *
   * After any error from `receive`, discard this replica and build a new one
   * from a confirmed snapshot and the log after it, handing it `history`.
   *
   * @throws Error if `seq` is not the next number, or if this replica's own
   *   operations come back out of the order it sent them in: the connection
   *   broke its promise. Nothing has changed yet when these are thrown.
   * @throws Error if rolling back an unconfirmed operation fails, which means
   *   an inverse was not exact: a bug in this package. Some inverses may have
   *   been applied already, so the view is no longer trustworthy, and the
   *   replica marks itself `broken`.
   */
  receive(message: Sequenced): ReceiveResult {
    this.checkUsable();
    if (message.seq !== this.confirmedSeq + 1) {
      throw new Error(`expected seq ${this.confirmedSeq + 1}, received ${message.seq}`);
    }
    const { operation } = message;
    // A malformed entry in the log is still an entry: every replica applies it, and it changes nothing.
    const id: unknown = typeof operation === 'object' && operation !== null ? operation.id : undefined;
    const head = this.pending[0];
    if (head?.sent === true && head.operation.id === id) {
      // Everything before it is confirmed, so the view already has it in the right place.
      this.pending.shift();
      this.confirmedSeq = message.seq;
      this.settle(head);
      if (head.inverse === null) {
        // It had no effect, so there is nothing to undo (`spikes/session-model` finding 4).
        if (head.entry !== null) this.forget(head.entry);
        return { kind: 'rejected', operation: head.operation, reason: head.reason ?? 'invalid', send: this.release() };
      }
      return { kind: 'confirmed', operation: head.operation, send: this.release() };
    }
    if (this.pending.some((entry) => entry.operation.id === id)) {
      throw new Error(`own operation ${String(id)} was confirmed out of order`);
    }

    this.rollBack(this.state);
    const result = this.state.apply(operation);
    this.confirmedSeq = message.seq;
    for (const entry of this.pending) {
      const target = entry.reverses;
      if (target !== null && target.inverse === null) {
        // What it would reverse has no effect now, so neither has it.
        this.record(entry, null, 'target-missing');
        continue;
      }
      if (target !== null) entry.operation = { ...entry.operation, change: target.inverse as Change };
      const again = this.state.apply(entry.operation);
      this.record(entry, again.applied ? again.inverse : null, again.applied ? null : again.reason);
    }
    return { kind: 'remote', applied: result.applied, send: [] };
  }

  /**
   * The confirmed state: what every replica that has received the same part of
   * the log holds. Costs time in proportion to the session's size; for saving
   * and for tests, not for every frame.
   */
  confirmedSnapshot(): SessionSnapshot {
    this.checkUsable();
    const copy = this.state.clone();
    this.rollBack(copy);
    return copy.snapshot();
  }

  // -------------------------------------------------------------------------

  /** Apply a change to the view as this member's next operation and keep it as unconfirmed. */
  private make(
    change: Change,
    reverses: Pending | null,
    recorded: boolean,
  ):
    | { readonly applied: true; readonly pending: Pending; readonly inverse: Change }
    | { readonly applied: false; readonly reason: RejectReason } {
    const operation: Operation = { id: this.nextOpId(), author: this.author, change };
    const result = this.state.apply(operation);
    if (!result.applied) return result;
    const pending: Pending = {
      operation,
      inverse: result.inverse,
      reason: null,
      entry: recorded ? { opId: operation.id, inverse: result.inverse } : null,
      reverses,
      sent: false,
    };
    this.pending.push(pending);
    return { applied: true, pending, inverse: result.inverse };
  }

  /**
   * Take entries off `from` until one applies; the inverse of the one that does
   * goes on `to`. Unconfirmed entries with no effect in the view go back on
   * `from`, in their places.
   */
  private reverse(from: Entry[], to: Entry[]): UndoResult {
    this.checkUsable();
    const skipped: OpId[] = [];
    const kept: Entry[] = [];
    let result: UndoResult = { applied: false, skipped };
    for (let entry = from.pop(); entry !== undefined; entry = from.pop()) {
      // Only an unconfirmed operation's inverse can be null: it has no effect in the view for now, but
      // the relay's order may yet give it one, as when someone else's removal of its target is undone.
      if (entry.inverse === null) {
        kept.push(entry);
        continue;
      }
      const made = this.make(entry.inverse, this.findPending(entry.opId) ?? null, true);
      if (made.applied && made.pending.entry !== null) {
        to.push(made.pending.entry);
        result = { applied: true, operation: made.pending.operation, reverses: entry.opId, skipped, send: this.release() };
        break;
      }
      skipped.push(entry.opId);
    }
    from.push(...kept.reverse());
    return result;
  }

  /** The operation `confirmed` was just confirmed. Undos of it waiting on it can go, or go away. */
  private settle(confirmed: Pending): void {
    if (confirmed.inverse !== null) {
      for (const entry of this.pending) {
        if (entry.reverses !== confirmed) continue;
        // Its inverse is now the one from the shared order, and so is the undo's change.
        entry.reverses = null;
        entry.operation = { ...entry.operation, change: confirmed.inverse };
      }
      return;
    }
    // It never took effect, so there is nothing to reverse: drop the undo held on it, a redo
    // held on that undo, and so on. None was applied (each one's inverse is null too), so
    // taking them out leaves the view as it is.
    const gone = new Set<Pending>([confirmed]);
    for (let i = 0; i < this.pending.length; ) {
      const entry = this.pending[i] as Pending;
      if (entry.reverses !== null && gone.has(entry.reverses)) {
        gone.add(entry);
        this.pending.splice(i, 1);
        if (entry.entry !== null) this.forget(entry.entry);
      } else {
        i += 1;
      }
    }
  }

  /** Own operations that can now be sent: every one not yet sent, up to the first still held back. */
  private release(): Operation[] {
    const out: Operation[] = [];
    for (const entry of this.pending) {
      if (entry.reverses !== null) break;
      if (!entry.sent) {
        entry.sent = true;
        out.push(entry.operation);
      }
    }
    return out;
  }

  private record(entry: Pending, inverse: Change | null, reason: RejectReason | null): void {
    entry.inverse = inverse;
    entry.reason = reason;
    if (entry.entry !== null) entry.entry.inverse = inverse;
  }

  private findPending(opId: OpId): Pending | undefined {
    return this.pending.find((entry) => entry.operation.id === opId);
  }

  private forget(entry: Entry): void {
    for (const stack of [this.undoStack, this.redoStack]) {
      const index = stack.indexOf(entry);
      if (index >= 0) stack.splice(index, 1);
    }
  }

  /** Undo the unconfirmed operations on `target`, newest first. */
  private rollBack(target: Session): void {
    for (let i = this.pending.length - 1; i >= 0; i -= 1) {
      const entry = this.pending[i];
      if (entry === undefined || entry.inverse === null) continue;
      const undone = target.apply({ id: entry.operation.id, author: this.author, change: entry.inverse });
      if (!undone.applied) {
        this.brokenBy = `rolling back ${entry.operation.id} failed (${undone.reason}); its inverse was not exact`;
        throw new Error(`${this.brokenBy}. This replica is now broken; build a new one`);
      }
    }
  }

  private checkUsable(): void {
    if (this.brokenBy !== null) throw new Error(`this replica is broken (${this.brokenBy}); build a new one`);
  }
}

/**
 * Puts operations in order when there is no relay, as when working solo
 * (ADR-0004). Hand each operation a replica gives to send to `next`, and the
 * result to the replica's `receive`.
 */
export class Sequencer {
  private last: number;

  constructor(seq = 0) {
    this.last = seq;
  }

  get seq(): number {
    return this.last;
  }

  next(operation: Operation): Sequenced {
    this.last += 1;
    return { seq: this.last, operation };
  }
}

/**
 * Operation ids `prefix1`, `prefix2` and so on. The prefix must be unique to this
 * replica for this run of the app, such as the member id and a random value, so
 * ids never repeat when the app starts again.
 */
export function opIdSource(prefix: string): () => OpId {
  let n = 0;
  return () => `${prefix}${(n += 1)}`;
}
