// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * One person's copy of the session, kept in step with the shared order
 * (ADR-0004).
 *
 * A replica holds one mutable state, its view: every operation the relay has
 * ordered so far, plus this person's own operations that it has not ordered
 * yet. Local edits apply at once. When someone else's operation arrives, the
 * replica rolls its unconfirmed operations back with their inverses, applies
 * the new one, and applies its own again on top. That costs time in proportion
 * to the number of unconfirmed operations, whatever the size of the session.
 * When its own operation comes back from the relay in order, nothing needs
 * rolling back at all.
 */

import { Session, type SessionSnapshot, type SessionView } from './session';
import type { Change, MemberId, OpId, Operation, RejectReason } from './types';

/** An operation after the relay, or a `Sequencer` when working solo, has given it its place in the order. */
export interface Sequenced {
  /** 1 for the session's first operation, then 2, 3 and so on, with no gaps. */
  readonly seq: number;
  readonly operation: Operation;
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
}

export type SubmitResult =
  | {
      readonly applied: true;
      /** Send this to the relay. */
      readonly operation: Operation;
      /**
       * The inverse against the view as it is now. If another person's operation
       * is ordered first, the true inverse can change; the replica keeps track.
       */
      readonly inverse: Change;
    }
  | { readonly applied: false; readonly reason: RejectReason };

export type ReceiveResult =
  /** Someone else's operation. `applied` says whether it changed anything. */
  | { readonly kind: 'remote'; readonly applied: boolean }
  /** One of this replica's own operations, confirmed, and it took effect. */
  | { readonly kind: 'confirmed'; readonly operation: Operation }
  /**
   * One of this replica's own operations, confirmed, but it had no effect,
   * usually because someone else removed its target first. It is already gone
   * from the view. The interface should tell the person.
   */
  | { readonly kind: 'rejected'; readonly operation: Operation; readonly reason: RejectReason };

interface Pending {
  readonly operation: Operation;
  /** The inverse against the view, or null when the operation has no effect there. */
  inverse: Change | null;
  /** Why it has no effect there, when it has none. */
  reason: RejectReason | null;
}

export class Replica {
  readonly author: MemberId;
  private readonly nextOpId: () => OpId;
  private readonly state: Session;
  private confirmedSeq: number;
  private readonly pending: Pending[] = [];

  constructor(options: ReplicaOptions) {
    this.author = options.author;
    this.nextOpId = options.nextOpId;
    this.state = options.session ?? Session.empty();
    this.confirmedSeq = options.seq ?? 0;
  }

  /** What the interface draws: the confirmed state plus this person's unconfirmed operations. */
  get view(): SessionView {
    return this.state;
  }

  /** The sequence number of the last operation received from the relay. */
  get seq(): number {
    return this.confirmedSeq;
  }

  /** Own operations sent but not yet ordered by the relay, oldest first. */
  get unconfirmed(): readonly Operation[] {
    return this.pending.map((entry) => entry.operation);
  }

  /**
   * Make an edit. It applies to the view at once. If it applied, send the
   * returned operation to the relay; if not, nothing changed and there is
   * nothing to send.
   */
  submit(change: Change): SubmitResult {
    const operation: Operation = { id: this.nextOpId(), author: this.author, change };
    const result = this.state.apply(operation);
    if (!result.applied) return result;
    this.pending.push({ operation, inverse: result.inverse, reason: null });
    return { applied: true, operation, inverse: result.inverse };
  }

  /**
   * Take the next operation in the shared order.
   *
   * @throws Error if `seq` is not the next number, or if this replica's own
   *   operations come back out of the order it sent them in. Either means the
   *   connection broke its promise, and the replica must be rebuilt.
   */
  receive(message: Sequenced): ReceiveResult {
    if (message.seq !== this.confirmedSeq + 1) {
      throw new Error(`expected seq ${this.confirmedSeq + 1}, received ${message.seq}`);
    }
    const { operation } = message;
    // A malformed entry in the log is still an entry: every replica applies it, and it changes nothing.
    const id: unknown = typeof operation === 'object' && operation !== null ? operation.id : undefined;
    const head = this.pending[0];
    if (head !== undefined && head.operation.id === id) {
      // Everything before it is confirmed, so the view already has it in the right place.
      this.pending.shift();
      this.confirmedSeq = message.seq;
      return head.inverse === null
        ? { kind: 'rejected', operation: head.operation, reason: head.reason ?? 'invalid' }
        : { kind: 'confirmed', operation: head.operation };
    }
    if (this.pending.some((entry) => entry.operation.id === id)) {
      throw new Error(`own operation ${String(id)} was confirmed out of order`);
    }

    this.rollBack(this.state);
    const result = this.state.apply(operation);
    this.confirmedSeq = message.seq;
    for (const entry of this.pending) {
      const again = this.state.apply(entry.operation);
      entry.inverse = again.applied ? again.inverse : null;
      entry.reason = again.applied ? null : again.reason;
    }
    return { kind: 'remote', applied: result.applied };
  }

  /**
   * The confirmed state: what every replica that has received the same part of
   * the log holds. Costs time in proportion to the session's size; for saving
   * and for tests, not for every frame.
   */
  confirmedSnapshot(): SessionSnapshot {
    const copy = this.state.clone();
    this.rollBack(copy);
    return copy.snapshot();
  }

  /** Undo the unconfirmed operations on `target`, newest first. */
  private rollBack(target: Session): void {
    for (let i = this.pending.length - 1; i >= 0; i -= 1) {
      const entry = this.pending[i];
      if (entry === undefined || entry.inverse === null) continue;
      const undone = target.apply({ id: entry.operation.id, author: this.author, change: entry.inverse });
      if (!undone.applied) {
        throw new Error(`rolling back ${entry.operation.id} failed (${undone.reason}); its inverse was not exact`);
      }
    }
  }
}

/**
 * Puts operations in order when there is no relay, as when working solo
 * (ADR-0004). Hand each submitted operation to `next` and the result to the
 * replica's `receive`.
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
