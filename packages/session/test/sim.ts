// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import {
  type Change,
  fileTimeAt,
  type Operation,
  opIdSource,
  type ReceiveResult,
  Replica,
  type Sequenced,
  Sequencer,
  Session,
  type SessionView,
  type SubmitResult,
  trackPositionAt,
  type UndoResult,
} from '../src';
import { hash, PROJECT, SEED, verifier } from './helpers';

/**
 * An in-memory lobby: a sequencer standing in for the relay, any number of
 * replicas, and a queue each way per replica. The test decides when each message
 * is delivered, which is how network delay is simulated. Each link keeps its own
 * order, as a WebSocket does.
 */
export class Sim {
  readonly log: Sequenced[] = [];
  readonly clients: Replica[];
  /** For each replica, the inverses of its own edits, so tests can send them later as undo would. */
  readonly inverses: Change[][];
  /** For each replica, how many of its own operations came back rejected. */
  readonly rejected: number[];
  /** For each replica, what each delivery to it returned. */
  readonly received: ReceiveResult[][];
  /** Every undo and redo operation any replica made, by id, with the id of the operation it reverses. */
  readonly undos = new Map<string, string>();
  private readonly sequencer = new Sequencer();
  private readonly up: Operation[][];
  private readonly down: Sequenced[][];

  constructor(count: number, seed: readonly Change[] = SEED) {
    this.up = Array.from({ length: count }, () => []);
    this.down = Array.from({ length: count }, () => []);
    this.inverses = Array.from({ length: count }, () => []);
    this.rejected = Array.from({ length: count }, () => 0);
    this.received = Array.from({ length: count }, () => []);
    this.clients = this.up.map(
      (_queue, i) => new Replica({ author: `m${i}`, nextOpId: opIdSource(`m${i}:`), session: Session.empty(PROJECT) }),
    );
    // Everyone is a member, then the shared starting material, all authored by the first member.
    const setup: Change[] = this.clients.map((client, i) => ({
      type: 'member.add',
      member: {
        id: client.author,
        name: `Member ${i}`,
        colour: `#${(0x123456 * (i + 1)).toString(16).slice(-6).padStart(6, '0')}`,
        verifier: verifier(i),
      },
    }));
    setup.push(...seed);
    setup.forEach((change, i) => this.order({ id: `seed:${i}`, author: 'm0', change }));
    this.drain();
  }

  client(i: number): Replica {
    const replica = this.clients[i % this.clients.length];
    if (replica === undefined) throw new Error('no clients');
    return replica;
  }

  /** Make an edit on one replica and queue it for the relay if it applied. */
  submit(i: number, change: Change): SubmitResult {
    const index = i % this.clients.length;
    const result = this.client(index).submit(change);
    if (result.applied) {
      this.up[index]?.push(...result.send);
      this.inverses[index]?.push(result.inverse);
    }
    return result;
  }

  /** Press undo on one replica, and queue what it sends. */
  undo(i: number): UndoResult {
    return this.reverse(i, (replica) => replica.undo());
  }

  /** Press redo on one replica, and queue what it sends. */
  redo(i: number): UndoResult {
    return this.reverse(i, (replica) => replica.redo());
  }

  /**
   * Replace a replica with a new one for the same member, as when the app
   * starts again: built from its confirmed state, with new operation ids, and
   * given the old one's undo history unless `keepHistory` is false. Anything it
   * had not sent is lost; whatever it had sent still reaches the relay.
   */
  restart(i: number, run: string, keepHistory = true): Replica {
    const index = i % this.clients.length;
    const old = this.client(index);
    const fresh = new Replica({
      author: old.author,
      nextOpId: opIdSource(`${old.author}:${run}:`),
      session: Session.fromSnapshot(old.confirmedSnapshot()),
      seq: old.seq,
      ...(keepHistory ? { history: old.history } : {}),
    });
    this.clients[index] = fresh;
    return fresh;
  }

  /** Deliver one replica's oldest unsent operation to the relay. */
  deliverUp(i: number): boolean {
    const operation = this.up[i % this.up.length]?.shift();
    if (operation === undefined) return false;
    this.order(operation);
    return true;
  }

  /** Deliver the oldest undelivered relay message to one replica. */
  deliverDown(i: number): boolean {
    const index = i % this.down.length;
    const message = this.down[index]?.shift();
    if (message === undefined) return false;
    const result = this.client(index).receive(message);
    this.up[index]?.push(...result.send);
    this.received[index]?.push(result);
    if (result.kind === 'rejected') this.rejected[index] = (this.rejected[index] ?? 0) + 1;
    return true;
  }

  /** Deliver everything until the network is quiet. */
  drain(): void {
    let moved = true;
    while (moved) {
      moved = false;
      for (let i = 0; i < this.clients.length; i += 1) {
        while (this.deliverUp(i)) moved = true;
        while (this.deliverDown(i)) moved = true;
      }
    }
  }

  private reverse(i: number, press: (replica: Replica) => UndoResult): UndoResult {
    const index = i % this.clients.length;
    const result = press(this.client(index));
    if (result.applied) {
      this.undos.set(result.operation.id, result.reverses);
      this.up[index]?.push(...result.send);
    }
    return result;
  }

  private order(operation: Operation): void {
    const message = this.sequencer.next(operation);
    this.log.push(message);
    for (const queue of this.down) queue.push(message);
  }
}

// ---------------------------------------------------------------------------
// Random edits
// ---------------------------------------------------------------------------

export type Dice = readonly [number, number, number, number, number];

export const dice: fc.Arbitrary<Dice> = fc.tuple(fc.nat(9999), fc.nat(9999), fc.nat(9999), fc.nat(9999), fc.nat(9999));

export const KINDS = [
  'clip.add',
  'clip.add',
  'clip.move',
  'clip.move',
  'clip.trim',
  'clip.trim',
  'clip.split',
  'clip.gain',
  'clip.fade',
  'clip.remove',
  'track.add',
  'track.remove',
  'track.rename',
  'track.move',
  'track.move',
  'member.add',
  'member.rename',
  'member.recolour',
  'member.remove',
  'tempo.set',
  'timeSignature.set',
] as const;

export type Kind = (typeof KINDS)[number];

const pick = <T>(items: readonly T[], n: number): T | undefined => items[n % Math.max(items.length, 1)];

/**
 * Turn five random numbers into an edit against whatever this replica can see.
 * Targets come from its own view, which may be stale: that is how conflicting
 * edits arise in real use. `kinds` biases the mix.
 */
export function randomChange(
  view: SessionView,
  [a, b, c, d, e]: Dice,
  freshId: () => string,
  kinds: readonly Kind[] = KINDS,
): Change | undefined {
  const kind = pick(kinds, a);
  const trackId = pick(view.trackOrder, b);
  const otherTrackId = pick(view.trackOrder, c);
  const clip = pick(
    [...view.clips.values()].sort((x, y) => (x.id < y.id ? -1 : 1)),
    b,
  );
  const memberId = pick([...view.members.keys()].sort(), b);
  switch (kind) {
    case 'clip.add':
      return trackId === undefined
        ? undefined
        : {
            type: 'clip.add',
            clip: {
              id: freshId(),
              trackId,
              start: (c % 64) * 240,
              length: 240 * (1 + (d % 16)),
              offset: (e % 4) * 0.25,
              gain: (e % 13) - 6,
              fadeIn: (d % 3) * 120,
              fadeOut: (e % 3) * 120,
              sample:
                e % 2 === 0
                  ? { fileHash: hash(d % 7), name: `s${d % 7}.wav` }
                  : { fileHash: hash(d % 7), audioHash: hash(100 + (d % 7)), name: `s${d % 7}.aif` },
            },
          };
    case 'clip.move':
      return clip === undefined || otherTrackId === undefined
        ? undefined
        : { type: 'clip.move', clipId: clip.id, trackId: otherTrackId, start: (d % 64) * 240 };
    case 'clip.trim': {
      if (clip === undefined) return undefined;
      // Where the person drops each edge, as the interface would send it. Some pass the start of the file and are refused.
      const at = (tick: number) => fileTimeAt(clip, tick, view.timeline.tempo);
      const to = at(clip.start + 240 * (1 + (d % 16)));
      const from = at(Math.max(0, clip.start + ((d % 9) - 4) * 240));
      if (c % 3 === 0) return { type: 'clip.trim', clipId: clip.id, to };
      if (c % 3 === 1) return { type: 'clip.trim', clipId: clip.id, from };
      return { type: 'clip.trim', clipId: clip.id, from, to };
    }
    case 'clip.split':
      return clip === undefined || clip.length < 2
        ? undefined
        : { type: 'clip.split', clipId: clip.id, newClipId: freshId(), at: 1 + (d % (clip.length - 1)) };
    case 'clip.gain':
      return clip === undefined ? undefined : { type: 'clip.gain', clipId: clip.id, gain: (d % 25) - 12 };
    case 'clip.fade':
      return clip === undefined
        ? undefined
        : { type: 'clip.fade', clipId: clip.id, fadeIn: (d % 5) * 120, fadeOut: (e % 5) * 120 };
    case 'clip.remove':
      return clip === undefined ? undefined : { type: 'clip.remove', clipId: clip.id };
    case 'track.add':
      return {
        type: 'track.add',
        track: {
          id: freshId(),
          name: `Track ${c % 10}`,
          position: trackPositionAt(view, d % (view.trackOrder.length + 1)),
        },
      };
    case 'track.remove':
      return trackId === undefined ? undefined : { type: 'track.remove', trackId };
    case 'track.rename':
      return trackId === undefined ? undefined : { type: 'track.rename', trackId, name: `Renamed ${c % 10}` };
    case 'track.move':
      return trackId === undefined
        ? undefined
        : { type: 'track.move', trackId, position: trackPositionAt(view, d % view.trackOrder.length, trackId) };
    case 'member.add':
      return {
        type: 'member.add',
        member: {
          id: freshId(),
          name: `Guest ${c % 10}`,
          colour: `#00${(d % 256).toString(16).padStart(2, '0')}ff`,
          verifier: verifier(e % 5),
        },
      };
    case 'member.rename':
      return memberId === undefined ? undefined : { type: 'member.rename', memberId, name: `Name ${c % 10}` };
    case 'member.recolour':
      return memberId === undefined
        ? undefined
        : { type: 'member.recolour', memberId, colour: `#${(d % 4096).toString(16).padStart(3, '0')}abc` };
    case 'member.remove':
      return memberId === undefined ? undefined : { type: 'member.remove', memberId };
    case 'tempo.set':
      return { type: 'tempo.set', tempo: 60 + (c % 120) + (d % 4) * 0.25 };
    case 'timeSignature.set':
      return { type: 'timeSignature.set', timeSignature: { numerator: 1 + (c % 12), denominator: 2 ** (d % 4) * 2 } };
    default:
      return undefined;
  }
}

/** Deterministic ids for one replica's new tracks, clips and members. */
export function idSource(prefix: string): () => string {
  let n = 0;
  return () => `${prefix}-${(n += 1)}`;
}

// ---------------------------------------------------------------------------
// Random runs
// ---------------------------------------------------------------------------

export type Step =
  | { kind: 'edit'; who: number; dice: Dice }
  | { kind: 'late'; who: number; pick: number }
  | { kind: 'undo'; who: number }
  | { kind: 'redo'; who: number }
  | { kind: 'up'; who: number }
  | { kind: 'down'; who: number };

const who = fc.nat(11);

/** Step weights: `late` sends an old inverse as a plain edit; `undo` and `redo` press the buttons. */
export function stepOf(weights: { readonly undo: number; readonly redo: number } = { undo: 2, redo: 1 }) {
  return fc.oneof(
    { weight: 6, arbitrary: fc.record({ kind: fc.constant('edit' as const), who, dice }) },
    { weight: 2, arbitrary: fc.record({ kind: fc.constant('late' as const), who, pick: fc.nat(999) }) },
    { weight: weights.undo, arbitrary: fc.record({ kind: fc.constant('undo' as const), who }) },
    { weight: weights.redo, arbitrary: fc.record({ kind: fc.constant('redo' as const), who }) },
    { weight: 3, arbitrary: fc.record({ kind: fc.constant('up' as const), who }) },
    { weight: 4, arbitrary: fc.record({ kind: fc.constant('down' as const), who }) },
  ) as fc.Arbitrary<Step>;
}

/** Long enough runs that every rejection reason and every kind of inverse turns up. */
export const stepLists = (maxLength: number, step: fc.Arbitrary<Step> = stepOf()): fc.Arbitrary<Step[]> =>
  fc.array(step, { maxLength, size: 'medium' });

/**
 * Run a random interleaving of edits, late inverses, undos, redos and message
 * deliveries, checking `afterEach` after every step.
 */
export function run(
  count: number,
  steps: readonly Step[],
  kinds: readonly Kind[] = KINDS,
  afterEach?: (sim: Sim) => void,
): Sim {
  const sim = new Sim(count);
  const ids = sim.clients.map((client) => idSource(client.author));
  for (const s of steps) {
    const index = s.who % count;
    if (s.kind === 'edit') {
      const fresh = ids[index];
      const change = fresh && randomChange(sim.client(index).view, s.dice, fresh, kinds);
      if (change) sim.submit(index, change);
    } else if (s.kind === 'late') {
      const mine = sim.inverses[index] ?? [];
      const change = mine[s.pick % Math.max(mine.length, 1)];
      if (change) sim.submit(index, change);
    } else if (s.kind === 'undo') sim.undo(index);
    else if (s.kind === 'redo') sim.redo(index);
    else if (s.kind === 'up') sim.deliverUp(index);
    else sim.deliverDown(index);
    afterEach?.(sim);
  }
  return sim;
}
