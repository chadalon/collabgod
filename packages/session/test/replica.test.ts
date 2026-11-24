// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it, vi } from 'vitest';
import { canonical, type Change, opIdSource, Replica, Sequencer, Session, trackPositionAt } from '../src';
import { newClip, trimTo } from './helpers';
import { Sim } from './sim';

const applied = (result: ReturnType<Replica['submit']>) => {
  if (!result.applied) throw new Error(`did not apply: ${result.reason}`);
  return result;
};

describe('a replica working solo', () => {
  it('applies an edit at once and confirms it through a local sequence', () => {
    const me = new Replica({ author: 'chad', nextOpId: opIdSource('chad:run1:') });
    const order = new Sequencer();
    const sent = applied(me.submit({ type: 'tempo.set', tempo: 128 }));

    expect(sent.operation).toEqual({ id: 'chad:run1:1', author: 'chad', change: { type: 'tempo.set', tempo: 128 } });
    expect(me.view.timeline.tempo).toBe(128);
    expect(me.unconfirmed).toHaveLength(1);

    expect(me.receive(order.next(sent.operation))).toEqual({ kind: 'confirmed', operation: sent.operation });
    expect(me.unconfirmed).toHaveLength(0);
    expect(me.seq).toBe(1);
    expect(me.view.timeline.rev.tempo).toBe('chad:run1:1');
  });

  it('sends nothing for an edit that does not apply, and says why', () => {
    const me = new Replica({ author: 'chad', nextOpId: opIdSource('c:') });
    expect(me.submit({ type: 'track.rename', trackId: 'nope', name: 'x' })).toEqual({
      applied: false,
      reason: 'target-missing',
    });
    expect(me.unconfirmed).toHaveLength(0);
  });

  it('starts from a saved state and sequence number', () => {
    const saved = Session.empty();
    saved.apply({ id: 'old:1', author: 'chad', change: { type: 'tempo.set', tempo: 90 } });
    const me = new Replica({ author: 'chad', nextOpId: opIdSource('c:'), session: saved, seq: 41 });
    expect(me.seq).toBe(41);
    expect(me.view.timeline.tempo).toBe(90);
    expect(() =>
      me.receive({ seq: 41, operation: { id: 'x', author: 'b', change: { type: 'tempo.set', tempo: 1 } } }),
    ).toThrow(/expected seq 42/);
  });
});

describe('a replica in a lobby', () => {
  it('reports its own edit as rejected when someone else removed the target first, and drops it from the view', () => {
    const sim = new Sim(2);
    const [a, b] = [sim.client(0), sim.client(1)];
    applied(sim.submit(0, { type: 'track.remove', trackId: 't1' }));
    applied(sim.submit(1, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 4800 }));
    expect(b.view.clips.get('k1')?.start).toBe(4800);

    // The relay orders A's removal first.
    sim.deliverUp(0);
    sim.deliverUp(1);
    sim.deliverDown(1);
    expect(b.view.clips.has('k1')).toBe(false);

    // B now receives its own move back from the relay.
    sim.deliverDown(1);
    expect(sim.received[1]?.at(-1)).toMatchObject({
      kind: 'rejected',
      reason: 'target-missing',
      operation: { author: 'm1' },
    });
    expect(b.unconfirmed).toHaveLength(0);

    sim.drain();
    expect(canonical(b.view)).toBe(canonical(a.view));
  });

  it('keeps a reordered track where its mover put it when someone else adds a track at the same time', () => {
    const sim = new Sim(2);
    must(sim, 0, {
      type: 'track.add',
      track: { id: 't3', name: 'Keys', position: trackPositionAt(sim.client(0).view, 2) },
    });
    sim.drain();
    expect(sim.client(0).view.trackOrder).toEqual(['t1', 't2', 't3']);

    // A moves Keys to between Drums and Bass. B adds Vocals at the top. B's add is ordered first.
    must(sim, 0, { type: 'track.move', trackId: 't3', position: trackPositionAt(sim.client(0).view, 1, 't3') });
    must(sim, 1, {
      type: 'track.add',
      track: { id: 'v', name: 'Vocals', position: trackPositionAt(sim.client(1).view, 0) },
    });
    sim.deliverUp(1);
    sim.deliverUp(0);
    sim.drain();

    for (const client of sim.clients) expect(client.view.trackOrder).toEqual(['v', 't1', 't3', 't2']);
  });

  it('keeps both trims when two people trim the two edges of one clip at the same time', () => {
    const sim = new Sim(2);
    must(sim, 0, trimTo(sim.client(0).view, 'k3', { start: 960 }));
    must(sim, 1, trimTo(sim.client(1).view, 'k3', { end: 2880 }));
    sim.deliverUp(1);
    sim.deliverUp(0);
    sim.drain();
    for (const client of sim.clients) {
      expect(client.view.clips.get('k3')).toMatchObject({ start: 960, length: 1920, offset: 0.5 });
    }
  });

  it('lets the later of two trims to the same edge win, in either order, rather than adding them up', () => {
    for (const later of [0, 1]) {
      const sim = new Sim(2);
      // One person trims a beat off the left edge, the other two beats.
      must(sim, 0, trimTo(sim.client(0).view, 'k3', { start: 960 }));
      must(sim, 1, trimTo(sim.client(1).view, 'k3', { start: 1920 }));
      sim.deliverUp(1 - later);
      sim.deliverUp(later);
      sim.drain();
      const beats = later === 0 ? 1 : 2;
      for (const client of sim.clients) {
        expect(client.view.clips.get('k3')).toMatchObject({
          start: 960 * beats,
          length: 3840 - 960 * beats,
          offset: 0.5 * beats,
        });
      }
    }
  });

  it('trims the same audio when someone moved the clip first, whichever way they moved it', () => {
    for (const shift of [-1920, 1920]) {
      const sim = new Sim(2);
      must(sim, 0, { type: 'clip.move', clipId: 'k3', trackId: 't2', start: 7680 });
      sim.drain();
      // A moves the clip half a bar; B, not having seen that yet, trims half a bar off its left edge.
      must(sim, 0, { type: 'clip.move', clipId: 'k3', trackId: 't2', start: 7680 + shift });
      must(sim, 1, trimTo(sim.client(1).view, 'k3', { start: 7680 + 1920 }));
      sim.deliverUp(0);
      sim.deliverUp(1);
      sim.drain();
      for (const client of sim.clients) {
        // Half a bar of audio is gone, and the clip stays where A put it, less the half bar.
        expect(client.view.clips.get('k3')).toMatchObject({ start: 7680 + shift + 1920, length: 1920, offset: 1 });
      }
    }
  });

  it('keeps a trim on the same sound when someone’s tempo change is ordered just before it', () => {
    const sim = new Sim(2);
    must(sim, 0, { type: 'tempo.set', tempo: 125 });
    // At 120 BPM, B cuts at one beat: half a second into the file.
    must(sim, 1, trimTo(sim.client(1).view, 'k3', { start: 960 }));
    sim.deliverUp(0);
    sim.deliverUp(1);
    sim.drain();
    // At 125 BPM that half second plays at tick 1,000, so that is where the clip now starts, on the same sound.
    for (const client of sim.clients) {
      expect(client.view.clips.get('k3')).toMatchObject({ start: 1000, offset: 0.5 });
    }
  });

  it('works the same with three members, all editing at once', () => {
    const sim = new Sim(3);
    must(sim, 0, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 960 });
    must(sim, 1, { type: 'clip.gain', clipId: 'k1', gain: -4 });
    must(sim, 2, { type: 'clip.split', clipId: 'k1', newClipId: 'k1b', at: 480 });
    // The split is ordered first, so the move and the gain change only the left part.
    sim.deliverUp(2);
    sim.deliverUp(0);
    sim.deliverUp(1);
    sim.drain();
    const views = sim.clients.map((client) => canonical(client.view));
    expect(new Set(views).size).toBe(1);
    expect(sim.client(0).view.clips.get('k1')).toMatchObject({ trackId: 't2', start: 960, length: 480, gain: -4 });
    expect(sim.client(0).view.clips.get('k1b')).toMatchObject({ trackId: 't1', start: 480, length: 480, gain: 0 });
  });

  it('marks every operation with its author', () => {
    const sim = new Sim(3);
    must(sim, 2, { type: 'member.rename', memberId: 'm2', name: 'Brother' });
    must(sim, 1, { type: 'track.rename', trackId: 't1', name: 'Kit' });
    sim.drain();
    expect(sim.log.slice(-2).map((message) => message.operation.author)).toEqual(['m1', 'm2']);
  });

  it('treats a malformed entry in the log as an entry that changes nothing', () => {
    const me = new Replica({ author: 'm', nextOpId: opIdSource('m:') });
    applied(me.submit({ type: 'tempo.set', tempo: 100 }));
    for (const junk of [null, { id: 'x' }, { id: 'y', author: 'z', change: { type: 'nope' } }]) {
      expect(me.receive({ seq: me.seq + 1, operation: junk as never })).toEqual({ kind: 'remote', applied: false });
    }
    expect(me.seq).toBe(3);
    expect(me.view.timeline.tempo).toBe(100);
    expect(me.unconfirmed).toHaveLength(1);
  });

  it('refuses its own operations out of the order it sent them', () => {
    const me = new Replica({ author: 'm', nextOpId: opIdSource('m:') });
    const first = applied(me.submit({ type: 'tempo.set', tempo: 100 }));
    const second = applied(me.submit({ type: 'tempo.set', tempo: 110 }));
    expect(first.operation.id).not.toBe(second.operation.id);
    expect(() => me.receive({ seq: 1, operation: second.operation })).toThrow(/out of order/);
  });
});

describe('a replica whose rollback fails', () => {
  it('marks itself broken and refuses to go on, so a half rolled-back view is never used', () => {
    const me = new Replica({ author: 'me', nextOpId: opIdSource('me:') });
    applied(me.submit({ type: 'tempo.set', tempo: 100 }));
    applied(me.submit({ type: 'tempo.set', tempo: 110 }));
    // Make the first inverse refuse, as an inexact one would. Only a bug in this package could do that.
    const spy = vi.spyOn(Session.prototype, 'apply').mockReturnValueOnce({ applied: false, reason: 'guard-failed' });
    const theirs = { id: 'them:1', author: 'them', change: { type: 'tempo.set', tempo: 90 } } as const;
    expect(() => me.receive({ seq: 1, operation: theirs })).toThrow(/rolling back me:2 failed.*build a new one/);
    spy.mockRestore();

    expect(me.broken).toBe(true);
    expect(() => me.receive({ seq: 1, operation: theirs })).toThrow(/broken/);
    expect(() => me.submit({ type: 'tempo.set', tempo: 120 })).toThrow(/broken/);
    expect(() => me.confirmedSnapshot()).toThrow(/broken/);
  });

  it('is not broken by a gap in the order, which changes nothing', () => {
    const me = new Replica({ author: 'me', nextOpId: opIdSource('me:') });
    expect(() =>
      me.receive({ seq: 2, operation: { id: 'x', author: 'them', change: { type: 'tempo.set', tempo: 90 } } }),
    ).toThrow(/expected seq 1/);
    expect(me.broken).toBe(false);
    expect(me.view.timeline.tempo).toBe(120);
  });
});

describe('the cost of receiving', () => {
  /** A replica with `pending` unconfirmed edits, counting every apply on its state. */
  function setUp(pending: number) {
    const session = Session.empty();
    session.apply({
      id: 's:t',
      author: 's',
      change: { type: 'track.add', track: { id: 't', name: 'T', position: 'i' } },
    });
    for (let i = 0; i < 50; i += 1) {
      session.apply({
        id: `s:${i}`,
        author: 's',
        change: { type: 'clip.add', clip: newClip(`c${i}`, 't', i * 960, 960) },
      });
    }
    const me = new Replica({ author: 'me', nextOpId: opIdSource('me:'), session });
    const mine = [];
    for (let i = 0; i < pending; i += 1) {
      mine.push(applied(me.submit({ type: 'clip.move', clipId: `c${i}`, trackId: 't', start: 100_000 + i })).operation);
    }
    const spy = vi.spyOn(Session.prototype, 'apply');
    return { me, mine, spy };
  }

  it('rolls back and reapplies exactly the unconfirmed edits for someone else’s operation', () => {
    for (const pending of [0, 1, 5, 20]) {
      const { me, spy } = setUp(pending);
      me.receive({
        seq: 1,
        operation: { id: 'them:1', author: 'them', change: { type: 'clip.gain', clipId: 'c40', gain: 2 } },
      });
      expect(spy).toHaveBeenCalledTimes(2 * pending + 1);
      spy.mockRestore();
    }
  });

  it('applies nothing at all when its own operation comes back in order', () => {
    const { me, mine, spy } = setUp(20);
    mine.forEach((operation, i) => me.receive({ seq: i + 1, operation }));
    expect(spy).not.toHaveBeenCalled();
    spy.mockRestore();
  });
});

function must(sim: Sim, who: number, change: Change): void {
  applied(sim.submit(who, change));
}
