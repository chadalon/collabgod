// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import {
  canonical,
  type Change,
  NO_REVISION,
  type Operation,
  opIdSource,
  Replica,
  Sequencer,
  Session,
  type SubmitResult,
  type UndoResult,
} from '../src';
import { PROJECT, trimTo, verifier } from './helpers';
import { Sim } from './sim';

const A = 0;
const B = 1;

function applied<R extends SubmitResult | UndoResult>(result: R): R & { applied: true } {
  if (!result.applied) throw new Error(`did not apply: ${JSON.stringify(result)}`);
  return result as R & { applied: true };
}

/** Make an edit in the lobby and return its operation id. */
function edit(sim: Sim, who: number, change: Change): string {
  return applied(sim.submit(who, change)).operation.id;
}

function expectSame(sim: Sim): void {
  sim.drain();
  const views = sim.clients.map((client) => canonical(client.view));
  expect(new Set(views).size).toBe(1);
}

/** One person alone: every operation goes through a local sequence at once. */
function alone() {
  const me = new Replica({ author: 'chad', nextOpId: opIdSource('chad:run1:'), session: Session.empty(PROJECT) });
  const order = new Sequencer();
  const confirm = (send: readonly Operation[]) => {
    for (const operation of send) me.receive(order.next(operation));
  };
  return {
    me,
    edit(change: Change): string {
      const result = applied(me.submit(change));
      confirm(result.send);
      return result.operation.id;
    },
    undo(): UndoResult {
      const result = me.undo();
      if (result.applied) confirm(result.send);
      return result;
    },
    redo(): UndoResult {
      const result = me.redo();
      if (result.applied) confirm(result.send);
      return result;
    },
  };
}

describe('undo and redo working solo', () => {
  it('undoes an edit and redoes it, revisions included', () => {
    const { me, edit: make, undo, redo } = alone();
    const id = make({ type: 'tempo.set', tempo: 128 });
    expect(me.canUndo).toBe(true);

    expect(undo()).toMatchObject({ applied: true, reverses: id, skipped: [] });
    expect(me.view.timeline).toMatchObject({ tempo: 120, rev: { tempo: NO_REVISION } });
    expect(me.canUndo).toBe(false);
    expect(me.canRedo).toBe(true);

    expect(redo()).toMatchObject({ applied: true, skipped: [] });
    expect(me.view.timeline).toMatchObject({ tempo: 128, rev: { tempo: id } });
    expect(me.canRedo).toBe(false);
    expect(me.unconfirmed).toHaveLength(0);
  });

  it('undoes several edits newest first, back to exactly where it started', () => {
    const { me, edit: make, undo } = alone();
    const empty = canonical(me.view);
    make({ type: 'track.add', track: { id: 't', name: 'Drums', position: 'i' } });
    make({ type: 'track.rename', trackId: 't', name: 'Kit' });
    const third = make({ type: 'tempo.set', tempo: 90 });
    expect(undo()).toMatchObject({ applied: true, reverses: third });
    expect(me.view.tracks.get('t')?.name).toBe('Kit');
    undo();
    expect(me.view.tracks.get('t')?.name).toBe('Drums');
    undo();
    expect(canonical(me.view)).toBe(empty);
    expect(undo()).toEqual({ applied: false, skipped: [] });
  });

  it('clears redo when a new edit is made', () => {
    const { me, edit: make, undo } = alone();
    make({ type: 'tempo.set', tempo: 100 });
    undo();
    expect(me.canRedo).toBe(true);
    make({ type: 'tempo.set', tempo: 110 });
    expect(me.canRedo).toBe(false);
  });

  it('has nothing to undo or redo on a new replica', () => {
    const { undo, redo } = alone();
    expect(undo()).toEqual({ applied: false, skipped: [] });
    expect(redo()).toEqual({ applied: false, skipped: [] });
  });

  it('does not offer to undo adding a member, which is never deleted', () => {
    const { me, edit: make, undo } = alone();
    make({ type: 'member.add', member: { id: 'chad', name: 'Chad', colour: '#ff8800', verifier: verifier(1) } });
    expect(me.canUndo).toBe(false);
    expect(undo().applied).toBe(false);
    expect(me.view.members.has('chad')).toBe(true);
  });

  it('works the same solo as in a lobby, operation for operation', () => {
    const sim = new Sim(2);
    // Solo starts from the same tracks and clips, as the same member, with the same operation ids.
    const me = new Replica({
      author: 'm0',
      nextOpId: opIdSource('m0:'),
      session: Session.fromSnapshot(sim.client(A).confirmedSnapshot()),
      seq: sim.client(A).seq,
    });
    const order = new Sequencer(me.seq);
    const confirm = (send: readonly Operation[]) => {
      for (const operation of send) me.receive(order.next(operation));
    };
    // Everything but `send`: in the lobby an undo of an edit the relay has not confirmed waits for it.
    const same = (result: SubmitResult | UndoResult) => {
      const { send: _send, ...rest } = result as typeof result & { send?: unknown };
      return rest;
    };
    const both = (press: () => UndoResult, inLobby: () => UndoResult) => {
      const solo = press();
      if (solo.applied) confirm(solo.send);
      expect(same(inLobby())).toEqual(same(solo));
    };
    const script: (Change | 'undo' | 'redo')[] = [
      { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 960 },
      { type: 'clip.gain', clipId: 'k1', gain: -3 },
      'undo',
      { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 960 },
      'undo',
      'redo',
      'undo',
      'undo',
    ];
    for (const step of script) {
      if (step === 'undo') both(() => me.undo(), () => sim.undo(A));
      else if (step === 'redo') both(() => me.redo(), () => sim.redo(A));
      else {
        const solo = applied(me.submit(step));
        confirm(solo.send);
        expect(same(sim.submit(A, step))).toEqual(same(solo));
      }
      // The lobby has not answered yet, and the views already agree.
      expect(canonical(sim.client(A).view)).toBe(canonical(me.view));
    }
    sim.drain();
    expect(canonical(sim.client(B).view)).toBe(canonical(me.view));
    expect(me.view.clips.get('k1')).toMatchObject({ trackId: 't1', start: 0, gain: 0 });
    expect(me.view.clips.has('k3b')).toBe(false);
  });
});

describe('undo and redo in a lobby', () => {
  it('undoes only my edits, leaving the other person’s in place', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    edit(sim, B, { type: 'clip.move', clipId: 'k2', trackId: 't1', start: 4800 });
    sim.drain();
    applied(sim.undo(A));
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')?.start).toBe(0);
    expect(sim.client(B).view.clips.get('k2')?.start).toBe(4800);
  });

  it('redoes my undo on every machine', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 480 });
    sim.drain();
    applied(sim.undo(A));
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')).toMatchObject({ trackId: 't1', start: 0 });
    applied(sim.redo(A));
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')).toMatchObject({ trackId: 't2', start: 480 });
  });

  it('does not undo my edit after the other person moves my clip away and back', () => {
    // Revision guards, not value guards (`session-model` finding 2): the values look untouched, but they wrote them.
    const sim = new Sim(2);
    const mine = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 3840 });
    sim.drain();
    edit(sim, B, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 0 });
    edit(sim, B, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 3840 });
    sim.drain();
    expect(sim.undo(A)).toEqual({ applied: false, skipped: [mine] });
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k1')).toMatchObject({ trackId: 't2', start: 3840 });
  });

  it('does not fire my older undo when I move a clip to 35 and back to 0 and the other person trims it to start at 35', () => {
    const sim = new Sim(2);
    const there = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 35 });
    const back = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 0 });
    sim.drain();
    edit(sim, B, trimTo(sim.client(B).view, 'k1', { start: 35 }));
    sim.drain();
    const trimmed = sim.client(A).view.clips.get('k1');
    expect(trimmed?.start).toBe(35);
    // The start is 35 again, the value my first move left, but the other person wrote it.
    expect(sim.undo(A)).toEqual({ applied: false, skipped: [back, there] });
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k1')).toEqual(trimmed);
  });

  it('keeps the other person’s change to the same clip’s length when I undo my move: guards are per field', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 4800 });
    sim.drain();
    edit(sim, B, trimTo(sim.client(B).view, 'k1', { end: 4800 + 480 }));
    edit(sim, B, { type: 'clip.gain', clipId: 'k1', gain: -6 });
    sim.drain();
    applied(sim.undo(A));
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')).toMatchObject({ trackId: 't1', start: 0, length: 480, gain: -6 });
  });

  it('puts back the previous revision, which makes the undo before it, mine or theirs, apply again', () => {
    const sim = new Sim(2);
    const first = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 960 });
    sim.drain();
    applied(sim.undo(A));
    expect(sim.client(A).view.clips.get('k1')?.rev.start).toBe(first);
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: first });
    expect(sim.client(A).view.clips.get('k1')?.start).toBe(0);
    expectSame(sim);

    // B moves k2, then A moves it. A's undo gives B's revision back, so B's undo applies again.
    const theirs = edit(sim, B, { type: 'clip.move', clipId: 'k2', trackId: 't1', start: 2400 });
    sim.drain();
    edit(sim, A, { type: 'clip.move', clipId: 'k2', trackId: 't1', start: 2880 });
    sim.drain();
    applied(sim.undo(A));
    sim.drain();
    expect(sim.client(B).view.clips.get('k2')).toMatchObject({ start: 2400, rev: { start: theirs } });
    expect(sim.undo(B)).toMatchObject({ applied: true, reverses: theirs, skipped: [] });
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k2')?.start).toBe(1920);
  });

  it('skips my edit the other person has since changed, and undoes my older edit still in effect', () => {
    const sim = new Sim(2);
    const rename = edit(sim, A, { type: 'track.rename', trackId: 't2', name: 'Sub' });
    const move = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    sim.drain();
    edit(sim, B, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 960 });
    sim.drain();
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: rename, skipped: [move] });
    expectSame(sim);
    expect(sim.client(B).view.tracks.get('t2')?.name).toBe('Bass');
    expect(sim.client(B).view.clips.get('k1')?.start).toBe(960);
  });

  it('restores a track I removed, with the other person’s clips on it', () => {
    const sim = new Sim(2);
    edit(sim, B, { type: 'clip.gain', clipId: 'k1', gain: 3 });
    sim.drain();
    const before = sim.client(A).view.clipsOnTrack('t1');
    edit(sim, A, { type: 'track.remove', trackId: 't1' });
    sim.drain();
    expect(sim.client(B).view.clips.has('k1')).toBe(false);
    applied(sim.undo(A));
    expectSame(sim);
    expect(sim.client(B).view.trackOrder).toEqual(['t1', 't2']);
    expect(sim.client(B).view.clipsOnTrack('t1')).toEqual(before);
  });
});

describe('undoing an edit to something the other person has since removed', () => {
  it('skips my edit to a clip they removed, and undoes my older edit still in effect', () => {
    const sim = new Sim(2);
    const rename = edit(sim, A, { type: 'track.rename', trackId: 't2', name: 'Sub' });
    const move = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    sim.drain();
    edit(sim, B, { type: 'clip.remove', clipId: 'k1' });
    sim.drain();
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: rename, skipped: [move] });
    expectSame(sim);
    expect(sim.client(A).view.clips.has('k1')).toBe(false);
  });

  it('forgets the skipped edit, even if they then bring the clip back', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    sim.drain();
    edit(sim, B, { type: 'clip.remove', clipId: 'k1' });
    sim.drain();
    expect(sim.undo(A).applied).toBe(false);
    applied(sim.undo(B));
    sim.drain();
    // The clip is back exactly as my move left it, but my move is no longer in my history.
    expect(sim.client(A).view.clips.get('k1')?.start).toBe(480);
    expect(sim.client(A).canUndo).toBe(false);
    expect(sim.undo(A).applied).toBe(false);
  });

  it('skips my removal of a clip on a track they then removed, rather than putting the clip nowhere', () => {
    const sim = new Sim(2);
    const removal = edit(sim, A, { type: 'clip.remove', clipId: 'k1' });
    sim.drain();
    edit(sim, B, { type: 'track.remove', trackId: 't1' });
    sim.drain();
    expect(sim.undo(A)).toEqual({ applied: false, skipped: [removal] });
    expectSame(sim);
    expect(sim.client(A).view.clips.has('k1')).toBe(false);
  });

  it('skips my clip add when they removed the clip, and my track add when they added a clip to the track', () => {
    const sim = new Sim(2);
    const track = edit(sim, A, { type: 'track.add', track: { id: 't3', name: 'Keys', position: 'z' } });
    sim.drain();
    edit(sim, B, { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 960 });
    edit(sim, B, { type: 'clip.move', clipId: 'k3b', trackId: 't3', start: 0 });
    sim.drain();
    expect(sim.undo(A)).toEqual({ applied: false, skipped: [track] });
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k3b')?.trackId).toBe('t3');
  });

  it('removes my edit from my history when it was dropped because they removed its target first', () => {
    const sim = new Sim(2);
    const older = edit(sim, A, { type: 'tempo.set', tempo: 100 });
    sim.drain();
    edit(sim, B, { type: 'clip.remove', clipId: 'k1' });
    edit(sim, A, { type: 'clip.gain', clipId: 'k1', gain: -2 });
    sim.deliverUp(B);
    sim.drain();
    expect(sim.rejected[A]).toBe(1);
    expect(sim.client(A).history.undo.map((entry) => entry.opId)).toEqual([older]);
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: older, skipped: [] });
    expectSame(sim);
  });
});

describe('undoing an edit the relay has not confirmed yet', () => {
  it('shows the undo at once and sends it once the edit is confirmed', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    const undo = applied(sim.undo(A));
    expect(undo.send).toEqual([]);
    expect(sim.client(A).view.clips.get('k1')?.start).toBe(0);
    sim.deliverUp(A);
    sim.deliverDown(A);
    // The move came back confirmed, which released the undo.
    expect(sim.received[A]?.at(-1)).toMatchObject({ kind: 'confirmed', send: [undo.operation] });
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')?.start).toBe(0);
  });

  it('keeps the other person’s edit that the relay put before mine', () => {
    // My undo must reverse my move as it landed in the shared order: after theirs.
    const sim = new Sim(2);
    const theirs = edit(sim, B, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 960 });
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 1920 });
    applied(sim.undo(A));
    expect(sim.client(A).view.clips.get('k1')?.start).toBe(0);
    sim.deliverUp(B);
    sim.deliverUp(A);
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k1')).toMatchObject({ start: 960, rev: { start: theirs } });
  });

  it('holds back later edits too, so they reach the relay in the order they were made', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    const undo = applied(sim.undo(A));
    const rename = applied(sim.submit(A, { type: 'track.rename', trackId: 't1', name: 'Kit' }));
    expect(rename.send).toEqual([]);
    sim.deliverUp(A);
    sim.deliverDown(A);
    expect(sim.received[A]?.at(-1)?.send).toEqual([undo.operation, rename.operation]);
    expectSame(sim);
    expect(sim.client(B).view.tracks.get('t1')?.name).toBe('Kit');
    expect(sim.client(B).view.clips.get('k1')?.start).toBe(0);
  });

  it('drops the undo, and a redo of it, when the edit comes back rejected', () => {
    const sim = new Sim(2);
    edit(sim, B, { type: 'clip.remove', clipId: 'k1' });
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    applied(sim.undo(A));
    applied(sim.redo(A));
    const later = applied(sim.submit(A, { type: 'tempo.set', tempo: 140 }));
    sim.deliverUp(B);
    sim.drain();
    expect(sim.rejected[A]).toBe(1);
    expect(sim.log.map((message) => message.operation.id).filter((id) => id.startsWith('m0:'))).toEqual([
      'm0:1',
      later.operation.id,
    ]);
    expect(sim.client(A).unconfirmed).toHaveLength(0);
    expect(sim.client(A).canRedo).toBe(false);
    expectSame(sim);
  });

  it('keeps an edit that has no effect on my screen only for now, until the relay says', () => {
    const sim = new Sim(2);
    // B removes k1 and undoes that; both reach the relay before A hears of either.
    edit(sim, B, { type: 'clip.remove', clipId: 'k1' });
    sim.deliverUp(B);
    sim.deliverDown(B);
    applied(sim.undo(B));
    sim.deliverUp(B);
    // A changes k1's gain. Then B's removal reaches A, so the change has no effect on A's screen for now.
    const gain = edit(sim, A, { type: 'clip.gain', clipId: 'k1', gain: -2 });
    sim.deliverDown(A);
    expect(sim.client(A).view.clips.has('k1')).toBe(false);
    // Undo passes over it, but keeps it.
    expect(sim.undo(A)).toEqual({ applied: false, skipped: [] });
    expect(sim.client(A).canUndo).toBe(true);
    // B's undo brings k1 back, and in the shared order the gain change takes effect.
    sim.drain();
    expect(sim.client(B).view.clips.get('k1')?.gain).toBe(-2);
    expect(sim.rejected[A]).toBe(0);
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: gain });
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')?.gain).toBe(0);
  });

  it('undoes an older edit past an unconfirmed one with no effect for now, and keeps the newer one', () => {
    const sim = new Sim(2);
    const older = edit(sim, A, { type: 'tempo.set', tempo: 100 });
    sim.drain();
    edit(sim, B, { type: 'clip.remove', clipId: 'k1' });
    sim.deliverUp(B);
    const gain = edit(sim, A, { type: 'clip.gain', clipId: 'k1', gain: -2 });
    sim.deliverDown(A);
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: older, skipped: [] });
    // The gain change is still on the undo side until the relay rejects it.
    expect(sim.client(A).canUndo).toBe(true);
    sim.drain();
    expect(sim.received[A]?.some((r) => r.kind === 'rejected' && r.operation.id === gain)).toBe(true);
    expect(sim.client(A).canUndo).toBe(false);
    expectSame(sim);
  });
});

describe('undo while the other person edits the same thing at the same moment', () => {
  function setUp() {
    const sim = new Sim(2);
    const older = edit(sim, A, { type: 'tempo.set', tempo: 100 });
    const move = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    sim.drain();
    // A presses undo while B moves the same clip; neither has seen the other's.
    applied(sim.undo(A));
    edit(sim, B, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 960 });
    return { sim, older, move };
  }

  it('when theirs is ordered first, my undo is rejected and their move stands', () => {
    const { sim, older } = setUp();
    sim.deliverUp(B);
    sim.drain();
    expect(sim.received[A]?.at(-1)).toMatchObject({ kind: 'rejected', reason: 'guard-failed' });
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k1')).toMatchObject({ trackId: 't2', start: 960 });
    // Nothing to redo, and the next undo goes on to my older edit.
    expect(sim.client(A).canRedo).toBe(false);
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: older });
    expectSame(sim);
  });

  it('when mine is ordered first, my undo applies and their later move wins; my redo is then skipped', () => {
    const { sim } = setUp();
    sim.deliverUp(A);
    sim.drain();
    expectSame(sim);
    expect(sim.client(A).view.clips.get('k1')).toMatchObject({ trackId: 't2', start: 960 });
    expect(sim.redo(A).applied).toBe(false);
    expectSame(sim);
  });

  it('when both undo their own edits to different fields of one clip at once, both undos apply', () => {
    for (const first of [A, B]) {
      const sim = new Sim(2);
      edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
      edit(sim, B, { type: 'clip.gain', clipId: 'k1', gain: -9 });
      sim.drain();
      applied(sim.undo(A));
      applied(sim.undo(B));
      sim.deliverUp(first);
      sim.drain();
      expectSame(sim);
      expect(sim.client(A).view.clips.get('k1')).toMatchObject({ start: 0, gain: 0 });
    }
  });

  it('when both undo edits to the same field at once, the order decides and both machines agree', () => {
    for (const first of [A, B]) {
      const sim = new Sim(2);
      edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
      sim.drain();
      edit(sim, B, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 960 });
      sim.drain();
      // A's move is no longer in effect, so A's undo finds nothing. B's undo puts A's move back.
      expect(sim.undo(A).applied).toBe(false);
      applied(sim.undo(B));
      sim.deliverUp(first);
      sim.drain();
      expectSame(sim);
      expect(sim.client(A).view.clips.get('k1')?.start).toBe(480);
    }
  });
});

describe('undo history belongs to the member, not the connection', () => {
  it('survives a dropped connection: what was missed arrives later and undo still reverses only my edit', () => {
    const sim = new Sim(2);
    const mine = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    sim.drain();
    // A is offline while B edits; B's edits wait for A.
    edit(sim, B, { type: 'clip.gain', clipId: 'k1', gain: -4 });
    sim.deliverUp(B);
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: mine });
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')).toMatchObject({ start: 0, gain: -4 });
  });

  it('carries over to a new replica for the same member, with new operation ids', () => {
    const sim = new Sim(2);
    const move = edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    const rename = edit(sim, A, { type: 'track.rename', trackId: 't1', name: 'Kit' });
    sim.drain();
    edit(sim, B, { type: 'clip.gain', clipId: 'k2', gain: 2 });
    sim.drain();
    sim.restart(A, 'run2');
    const undo = applied(sim.undo(A));
    expect(undo).toMatchObject({ reverses: rename, operation: { id: 'm0:run2:1', author: 'm0' } });
    expect(sim.undo(A)).toMatchObject({ applied: true, reverses: move });
    applied(sim.redo(A));
    expectSame(sim);
    const view = sim.client(B).view;
    expect(view.tracks.get('t1')?.name).toBe('Drums');
    expect(view.clips.get('k1')?.start).toBe(480);
    expect(view.clips.get('k2')?.gain).toBe(2);
  });

  it('puts back who made an older edit, on a replica built after that edit was overwritten', () => {
    const sim = new Sim(2);
    edit(sim, B, { type: 'clip.gain', clipId: 'k1', gain: -3 });
    sim.drain();
    edit(sim, A, { type: 'clip.gain', clipId: 'k1', gain: 6 });
    sim.drain();
    // B starts again from a snapshot that no longer names its own edit anywhere.
    sim.restart(B, 'run2');
    applied(sim.undo(A));
    expectSame(sim);
    expect(sim.client(B).view.clips.get('k1')).toMatchObject({ gain: -3, by: { gain: 'm1' }, createdBy: 'm0' });
  });

  it('is empty on a new replica not given it', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    sim.drain();
    sim.restart(A, 'run2', false);
    expect(sim.client(A).canUndo).toBe(false);
  });

  it('hands over confirmed entries only', () => {
    const sim = new Sim(2);
    const confirmed = edit(sim, A, { type: 'tempo.set', tempo: 100 });
    sim.drain();
    edit(sim, A, { type: 'tempo.set', tempo: 110 });
    expect(sim.client(A).history.undo.map((entry) => entry.opId)).toEqual([confirmed]);
  });

  it('refuses another member’s history', () => {
    const sim = new Sim(2);
    edit(sim, A, { type: 'tempo.set', tempo: 100 });
    sim.drain();
    const history = sim.client(A).history;
    const session = Session.empty(PROJECT);
    expect(() => new Replica({ author: 'm1', nextOpId: opIdSource('m1:x:'), session, history })).toThrow(/m0's, not m1's/);
  });

  it('works the same with three members', () => {
    const sim = new Sim(3);
    edit(sim, 0, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 960 });
    edit(sim, 1, { type: 'clip.gain', clipId: 'k1', gain: -4 });
    edit(sim, 2, { type: 'clip.fade', clipId: 'k1', fadeIn: 120, fadeOut: 0 });
    sim.drain();
    applied(sim.undo(1));
    expectSame(sim);
    expect(sim.client(0).view.clips.get('k1')).toMatchObject({ trackId: 't2', start: 960, gain: 0, fadeIn: 120 });
  });
});
