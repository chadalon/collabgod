// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { canonical, type Change, type Operation, Session } from '../src';
import { op, PROJECT, runs, SEED } from './helpers';
import { type Dice, dice, idSource, KINDS, randomChange } from './sim';

function seeded(): Session {
  const session = Session.empty(PROJECT);
  SEED.forEach((change, i) => session.apply({ id: `seed:${i}`, author: 'm0', change }));
  return session;
}

/**
 * A random step: a fresh edit, or one of the inverses collected so far, applied
 * late as an undo would be. Late inverses are where the guards matter.
 */
type Step = { kind: 'edit'; dice: Dice } | { kind: 'late'; pick: number };

const step: fc.Arbitrary<Step> = fc.oneof(
  { weight: 4, arbitrary: fc.record({ kind: fc.constant('edit' as const), dice }) },
  { weight: 1, arbitrary: fc.record({ kind: fc.constant('late' as const), pick: fc.nat(999) }) },
);

describe('every operation that applies has an exact inverse', () => {
  it('applying the inverse returns to exactly the state before, revisions included, and applying the operation again gives exactly the same state', () => {
    fc.assert(
      fc.property(fc.array(step, { maxLength: 100, size: 'medium' }), (steps) => {
        const session = seeded();
        const fresh = idSource('x');
        const inverses: Change[] = [];
        let n = 0;
        for (const s of steps) {
          const change =
            s.kind === 'edit' ? randomChange(session, s.dice, fresh) : inverses[s.pick % Math.max(inverses.length, 1)];
          if (change === undefined) continue;
          const operation: Operation = { id: `op:${(n += 1)}`, author: n % 2 === 0 ? 'm0' : 'm1', change };

          const before = canonical(session);
          const result = session.apply(operation);
          if (!result.applied) {
            expect(canonical(session)).toBe(before);
            continue;
          }
          const after = canonical(session);
          expect(session.checkInvariants()).toEqual([]);

          const undone = session.apply({ id: `undo:${n}`, author: operation.author, change: result.inverse });
          expect(undone.applied).toBe(true);
          expect(canonical(session)).toBe(before);

          const again = session.apply(operation);
          expect(again.applied).toBe(true);
          expect(canonical(session)).toBe(after);
          if (again.applied) inverses.push(again.inverse);
        }
      }),
      { numRuns: runs(400) },
    );
  });

  it('the same operations in the same order give the same state, whatever else happened in between and was rolled back', () => {
    fc.assert(
      fc.property(
        fc.array(dice, { maxLength: 80, size: 'medium' }),
        fc.array(dice, { maxLength: 10 }),
        (rolls, detour) => {
          const straight = seeded();
          const winding = seeded();
          const fresh = idSource('x');
          const detourIds = idSource('d');
          let n = 0;
          for (const roll of rolls) {
            // On `winding`, first apply a few edits and roll them back, which reorders its maps inside.
            const undo: Change[] = [];
            for (const extra of detour) {
              const change = randomChange(winding, extra, detourIds);
              const result = change && winding.apply(op(change));
              if (result?.applied) undo.push(result.inverse);
            }
            for (const inverse of undo.reverse()) expect(winding.apply(op(inverse)).applied).toBe(true);

            const change = randomChange(straight, roll, fresh);
            if (change === undefined) continue;
            const operation = op(change, 'm1', `op:${(n += 1)}`);
            const a = straight.apply(operation);
            const b = winding.apply(operation);
            expect(b).toEqual(a);
            expect(canonical(winding)).toBe(canonical(straight));
          }
        },
      ),
      { numRuns: runs(200) },
    );
  });
});

describe('extreme numbers', () => {
  /** Every kind of number an operation off the network could carry. */
  const extreme = fc.oneof(
    fc.double(),
    fc.integer(),
    fc.constantFrom(
      0,
      -0,
      0.5,
      -1,
      1e15,
      -1e15,
      1e300,
      Number.MAX_SAFE_INTEGER,
      Number.MAX_SAFE_INTEGER + 1,
      Number.MAX_VALUE,
      Number.MIN_VALUE,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      Number.NaN,
    ),
  );
  const clipId = fc.constantFrom('k1', 'k2', 'k3', 'n1', 'n2');
  const change: fc.Arbitrary<Change> = fc.oneof(
    fc.record(
      { type: fc.constant('clip.trim' as const), clipId, from: extreme, to: extreme },
      { requiredKeys: ['type', 'clipId'] },
    ),
    fc.record({
      type: fc.constant('clip.move' as const),
      clipId,
      trackId: fc.constantFrom('t1', 't2'),
      start: extreme,
    }),
    fc.record({
      type: fc.constant('clip.split' as const),
      clipId,
      newClipId: fc.constantFrom('n1', 'n2'),
      at: extreme,
    }),
    fc.record({ type: fc.constant('clip.gain' as const), clipId, gain: extreme }),
    fc.record({ type: fc.constant('clip.fade' as const), clipId, fadeIn: extreme, fadeOut: extreme }),
    fc.record({ type: fc.constant('tempo.set' as const), tempo: extreme }),
    fc
      .record({
        id: fc.constantFrom('n1', 'n2'),
        start: extreme,
        length: extreme,
        offset: extreme,
        gain: extreme,
        fadeIn: extreme,
        fadeOut: extreme,
      })
      .map((values): Change => ({
        type: 'clip.add',
        clip: { ...values, trackId: 't1', sample: { fileHash: '0'.repeat(64), name: 'x.wav' } },
      })),
  );

  it('never put a value into the session that breaks its rules, and every one that applies still has an exact inverse', () => {
    fc.assert(
      fc.property(fc.array(change, { maxLength: 40 }), (changes) => {
        const session = seeded();
        changes.forEach((c, i) => {
          const before = canonical(session);
          const result = session.apply({ id: `x:${i}`, author: 'm1', change: c });
          expect(session.checkInvariants()).toEqual([]);
          if (!result.applied) {
            expect(canonical(session)).toBe(before);
            return;
          }
          const after = canonical(session);
          expect(session.apply({ id: `u:${i}`, author: 'm1', change: result.inverse }).applied).toBe(true);
          expect(canonical(session)).toBe(before);
          expect(session.apply({ id: `x:${i}`, author: 'm1', change: c }).applied).toBe(true);
          expect(canonical(session)).toBe(after);
        });
      }),
      { numRuns: runs(1000) },
    );
  });
});

describe('malformed operations', () => {
  const junkChange = fc.oneof(
    fc.anything(),
    fc.record(
      {
        type: fc.constantFrom(
          ...KINDS,
          'clip.patch',
          'track.patch',
          'member.patch',
          'timeline.patch',
          'clip.unsplit',
          'clip.resplit',
          'member.unadd',
        ),
        clipId: fc.constantFrom('k1', 'k3', 'zz'),
        trackId: fc.constantFrom('t1', 'zz'),
        memberId: fc.string(),
      },
      { requiredKeys: ['type'] },
    ),
    fc
      .tuple(
        fc.constantFrom(...KINDS, 'clip.patch', 'track.patch', 'member.patch', 'timeline.patch'),
        fc.dictionary(fc.string(), fc.anything()),
      )
      .map(([type, rest]) => ({ ...rest, type })),
  );

  it('never throw and never change the session', () => {
    fc.assert(
      fc.property(junkChange, fc.anything(), (change, envelope) => {
        const session = seeded();
        const before = canonical(session);
        const result = session.apply({ id: 'junk', author: 'm1', change } as Operation);
        if (!result.applied) expect(canonical(session)).toBe(before);
        expect(session.checkInvariants()).toEqual([]);
        expect(session.apply(envelope as Operation).applied).toBe(false);
        expect(session.checkInvariants()).toEqual([]);
      }),
      { numRuns: runs(2000) },
    );
  });
});
