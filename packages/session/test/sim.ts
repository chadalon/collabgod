// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import {
  type Change,
  type Operation,
  opIdSource,
  type ReceiveResult,
  Replica,
  type Sequenced,
  Sequencer,
  type SessionView,
  type SubmitResult,
  trackPositionAt,
} from '../src';
import { hash, SEED } from './helpers';

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
  private readonly sequencer = new Sequencer();
  private readonly up: Operation[][];
  private readonly down: Sequenced[][];

  constructor(count: number, seed: readonly Change[] = SEED) {
    this.up = Array.from({ length: count }, () => []);
    this.down = Array.from({ length: count }, () => []);
    this.inverses = Array.from({ length: count }, () => []);
    this.rejected = Array.from({ length: count }, () => 0);
    this.received = Array.from({ length: count }, () => []);
    this.clients = this.up.map((_queue, i) => new Replica({ author: `m${i}`, nextOpId: opIdSource(`m${i}:`) }));
    // Everyone is a member, then the shared starting material, all authored by the first member.
    const setup: Change[] = this.clients.map((client, i) => ({
      type: 'member.add',
      member: {
        id: client.author,
        name: `Member ${i}`,
        colour: `#${(0x123456 * (i + 1)).toString(16).slice(-6).padStart(6, '0')}`,
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
      this.up[index]?.push(result.operation);
      this.inverses[index]?.push(result.inverse);
    }
    return result;
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
      const end = clip.start + 240 * (1 + (d % 16));
      // The left edge moves by up to a beat either way. Some of these pass the start of the file and are refused.
      const start = Math.max(0, clip.start + ((d % 9) - 4) * 240);
      if (c % 3 === 0) return { type: 'clip.trim', clipId: clip.id, end };
      if (c % 3 === 1) return { type: 'clip.trim', clipId: clip.id, start };
      return { type: 'clip.trim', clipId: clip.id, start, end };
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
        member: { id: freshId(), name: `Guest ${c % 10}`, colour: `#00${(d % 256).toString(16).padStart(2, '0')}ff` },
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
