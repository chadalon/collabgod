// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import {
  canonical,
  type Change,
  opIdSource,
  Replica,
  Sequencer,
  Session,
  type SessionView,
  type UndoResult,
} from '../src';
import { PROJECT, runs, SEED } from './helpers';
import { type Dice, dice, idSource, type Kind, KINDS, randomChange, run, Sim, stepLists, stepOf } from './sim';

/** Every field of every object as `[value, revision]` text, keyed by object and field. */
function fieldStates(view: SessionView): Map<string, string> {
  const states = new Map<string, string>();
  const put = (key: string, value: unknown, rev: unknown) => states.set(key, JSON.stringify([value, rev]));
  const { timeline } = view;
  put('timeline:tempo', timeline.tempo, timeline.rev.tempo);
  put('timeline:timeSignature', timeline.timeSignature, timeline.rev.timeSignature);
  for (const member of view.members.values()) {
    for (const field of ['name', 'colour', 'removed'] as const) {
      put(`member:${member.id}:${field}`, member[field], member.rev[field]);
    }
  }
  for (const track of view.tracks.values()) {
    for (const field of ['name', 'position'] as const) put(`track:${track.id}:${field}`, track[field], track.rev[field]);
  }
  for (const clip of view.clips.values()) {
    put(`clip:${clip.id}:sample`, clip.sample, null);
    for (const field of ['trackId', 'start', 'length', 'offset', 'gain', 'fadeIn', 'fadeOut'] as const) {
      put(`clip:${clip.id}:${field}`, clip[field], clip.rev[field]);
    }
  }
  return states;
}

/** The keys whose state differs between two field maps, including keys only one of them has. */
function changedKeys(before: Map<string, string>, after: Map<string, string>): Set<string> {
  const keys = new Set<string>();
  for (const [key, state] of before) if (after.get(key) !== state) keys.add(key);
  for (const key of after.keys()) if (!before.has(key)) keys.add(key);
  return keys;
}

/** The kinds of edit in a solo run. Adding a member is not undoable, so it would stop undo reaching the start. */
const SOLO_KINDS: readonly Kind[] = KINDS.filter((kind) => kind !== 'member.add');

type SoloStep = { kind: 'edit'; dice: Dice } | { kind: 'undo' } | { kind: 'redo' };

const soloStep: fc.Arbitrary<SoloStep> = fc.oneof(
  { weight: 5, arbitrary: fc.record({ kind: fc.constant('edit' as const), dice }) },
  { weight: 2, arbitrary: fc.record({ kind: fc.constant('undo' as const) }) },
  { weight: 1, arbitrary: fc.record({ kind: fc.constant('redo' as const) }) },
);

/** One person working alone: every operation is confirmed through a local sequence as soon as it is made. */
function solo(session: Session, seq: number) {
  const me = new Replica({ author: 'm0', nextOpId: opIdSource('m0:'), session, seq });
  const order = new Sequencer(seq);
  const confirm = (send: readonly { id: string }[] | undefined) => {
    for (const operation of send ?? []) me.receive(order.next(operation as never));
  };
  return {
    me,
    submit(change: Change) {
      const result = me.submit(change);
      if (result.applied) confirm(result.send);
      return result;
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

/** The session every run starts from, as a lobby's first member sees it. */
function start(): { session: Session; seq: number } {
  const sim = new Sim(2, SEED);
  const first = sim.client(0);
  return { session: Session.fromSnapshot(first.confirmedSnapshot()), seq: first.seq };
}

describe('undo and redo, property tests', () => {
  it('an undo or redo puts back exactly what the shared order had before the operation it reverses, and touches nothing else', () => {
    fc.assert(
      fc.property(stepLists(150, stepOf({ undo: 4, redo: 2 })), (steps) => {
        const sim = run(3, steps);
        sim.drain();
        const authors = new Map(sim.log.map(({ operation }) => [operation.id, operation.author]));
        const reversed = new Set(sim.undos.values());
        const around = new Map<string, { before: Map<string, string>; after: Map<string, string> }>();
        const session = Session.empty(PROJECT);
        for (const { operation } of sim.log) {
          const before = fieldStates(session);
          const applied = session.apply(operation).applied;
          const after = fieldStates(session);
          const target = sim.undos.get(operation.id);
          if (applied && target !== undefined) {
            // An undo or redo reverses an operation by the same member, and only one that took effect.
            expect(authors.get(target)).toBe(operation.author);
            const original = around.get(target);
            expect(original).toBeDefined();
            if (original === undefined) continue;
            for (const key of changedKeys(before, after)) {
              // It found each field as the reversed operation left it...
              expect(before.get(key)).toBe(original.after.get(key));
              // ...and left it as it was just before that operation, in the shared order.
              expect(after.get(key)).toBe(original.before.get(key));
            }
          }
          if (applied && reversed.has(operation.id)) around.set(operation.id, { before, after });
        }
      }),
      { numRuns: runs(200) },
    );
  });

  it('a member’s history only ever holds operations of theirs that took effect in the shared log', () => {
    fc.assert(
      fc.property(stepLists(150), (steps) => {
        const sim = run(2, steps);
        sim.drain();
        const session = Session.empty(PROJECT);
        const tookEffect = new Map<string, string>();
        for (const { operation } of sim.log) {
          if (session.apply(operation).applied) tookEffect.set(operation.id, operation.author);
        }
        for (const client of sim.clients) {
          const { undo, redo } = client.history;
          for (const entry of [...undo, ...redo]) expect(tookEffect.get(entry.opId)).toBe(client.author);
        }
      }),
      { numRuns: runs(200) },
    );
  });

  it('working solo, undoing everything gets back exactly to the start, and redoing everything exactly to the end', () => {
    fc.assert(
      fc.property(fc.array(soloStep, { maxLength: 80 }), (steps) => {
        const { session, seq } = start();
        const initial = canonical(session);
        const me = solo(session, seq);
        const fresh = idSource('m0');
        for (const step of steps) {
          if (step.kind === 'edit') {
            const change = randomChange(me.me.view, step.dice, fresh, SOLO_KINDS);
            if (change) me.submit(change);
          } else {
            // Alone, nobody else changes anything, so nothing is ever skipped.
            const result = step.kind === 'undo' ? me.undo() : me.redo();
            expect(result.skipped).toEqual([]);
          }
        }
        const end = canonical(me.me.view);
        let undone = 0;
        for (let result = me.undo(); result.applied; result = me.undo()) {
          expect(result.skipped).toEqual([]);
          undone += 1;
        }
        expect(canonical(me.me.view)).toBe(initial);
        // Redo as many times as undo went, leaving any undos made during the run undone, as they were at the end.
        for (let i = 0; i < undone; i += 1) {
          const result = me.redo();
          expect(result.applied).toBe(true);
          expect(result.skipped).toEqual([]);
        }
        expect(canonical(me.me.view)).toBe(end);
        expect(me.me.unconfirmed).toHaveLength(0);
      }),
      { numRuns: runs(300) },
    );
  });

  it('undo and redo behave the same solo as in a lobby where nobody else edits, whenever the relay answers', () => {
    type Step = SoloStep | { kind: 'up' } | { kind: 'down' };
    const lobbyStep: fc.Arbitrary<Step> = fc.oneof(
      { weight: 4, arbitrary: soloStep },
      { weight: 2, arbitrary: fc.record({ kind: fc.constant('up' as const) }) },
      { weight: 2, arbitrary: fc.record({ kind: fc.constant('down' as const) }) },
    );
    fc.assert(
      fc.property(fc.array(lobbyStep, { maxLength: 80 }), (steps) => {
        const sim = new Sim(2, SEED);
        const { session, seq } = start();
        const alone = solo(session, seq);
        const soloIds = idSource('m0');
        const lobbyIds = idSource('m0');
        const summary = (result: UndoResult) =>
          result.applied ? { reverses: result.reverses, skipped: result.skipped } : { skipped: result.skipped };
        for (const step of steps) {
          if (step.kind === 'up') sim.deliverUp(0);
          else if (step.kind === 'down') sim.deliverDown(0);
          else if (step.kind === 'edit') {
            const mine = randomChange(alone.me.view, step.dice, soloIds, KINDS);
            const theirs = randomChange(sim.client(0).view, step.dice, lobbyIds, KINDS);
            expect(theirs).toEqual(mine);
            if (mine) expect(alone.submit(mine).applied).toBe(sim.submit(0, mine).applied);
          } else if (step.kind === 'undo') {
            expect(summary(sim.undo(0))).toEqual(summary(alone.undo()));
          } else {
            expect(summary(sim.redo(0))).toEqual(summary(alone.redo()));
          }
          expect(canonical(sim.client(0).view)).toBe(canonical(alone.me.view));
        }
        sim.drain();
        expect(canonical(sim.client(1).view)).toBe(canonical(alone.me.view));
        expect(sim.client(0).history).toEqual(alone.me.history);
      }),
      { numRuns: runs(300) },
    );
  });
});
