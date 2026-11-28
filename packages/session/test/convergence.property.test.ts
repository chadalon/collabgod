// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { canonical, Replica, Session } from '../src';
import { fold, PROJECT, runs } from './helpers';
import { type Kind, KINDS, run, type Sim, stepLists } from './sim';

function expectConverged(sim: Sim): void {
  const truth = canonical(fold(sim.log.map((message) => message.operation)));
  for (const client of sim.clients) {
    expect(client.unconfirmed).toHaveLength(0);
    expect(canonical(client.view)).toBe(truth);
  }
}

const confirmedOf = (client: Replica): string => canonical(Session.fromSnapshot(client.confirmedSnapshot()));

describe('convergence', () => {
  it.each([
    [2, 500],
    [3, 300],
    [4, 200],
  ])('%i replicas always end in the state of the shared log, whatever the delivery order', (count, numRuns) => {
    fc.assert(
      fc.property(stepLists(150), (steps) => {
        const sim = run(count, steps);
        sim.drain();
        expectConverged(sim);
      }),
      { numRuns: runs(numRuns) },
    );
  });

  it('reordering tracks converges too, with every replica showing the same track order', () => {
    const reorderHeavy: readonly Kind[] = [
      'track.move',
      'track.move',
      'track.move',
      'track.add',
      'track.remove',
      'clip.add',
      'clip.move',
    ];
    fc.assert(
      fc.property(stepLists(150), (steps) => {
        const sim = run(3, steps, reorderHeavy);
        sim.drain();
        expectConverged(sim);
        const order = sim.client(0).view.trackOrder;
        for (const client of sim.clients) expect(client.view.trackOrder).toEqual(order);
      }),
      { numRuns: runs(300) },
    );
  });

  it('every state a replica ever shows is structurally valid', () => {
    fc.assert(
      fc.property(stepLists(60), (steps) => {
        run(3, steps, KINDS, (sim) => {
          for (const client of sim.clients) expect(client.view.checkInvariants()).toEqual([]);
        });
      }),
      { numRuns: runs(200) },
    );
  });

  it('a replica’s confirmed state is always the fold of the part of the log it has received', () => {
    fc.assert(
      fc.property(stepLists(60), (steps) => {
        run(2, steps, KINDS, (sim) => {
          for (const client of sim.clients) {
            const prefix = sim.log.slice(0, client.seq).map((message) => message.operation);
            expect(confirmedOf(client)).toBe(canonical(fold(prefix)));
          }
        });
      }),
      { numRuns: runs(150) },
    );
  });

  it('a replica’s view is always its confirmed state plus its own unconfirmed operations', () => {
    fc.assert(
      fc.property(stepLists(60), (steps) => {
        run(2, steps, KINDS, (sim) => {
          for (const client of sim.clients) {
            const confirmed = Session.fromSnapshot(client.confirmedSnapshot());
            expect(canonical(client.view)).toBe(canonical(fold(client.unconfirmed, confirmed)));
          }
        });
      }),
      { numRuns: runs(150) },
    );
  });

  it('a late joiner replaying the log matches everyone else', () => {
    fc.assert(
      fc.property(stepLists(120), (steps) => {
        const sim = run(2, steps);
        sim.drain();
        const late = new Replica({ author: 'late', nextOpId: () => 'unused', session: Session.empty(PROJECT) });
        for (const message of sim.log) late.receive(message);
        expect(canonical(late.view)).toBe(canonical(sim.client(0).view));
      }),
      { numRuns: runs(200) },
    );
  });

  it('an own operation reported as rejected had no effect in the shared log', () => {
    fc.assert(
      fc.property(stepLists(120), (steps) => {
        const sim = run(2, steps);
        sim.drain();
        // Replay the log; every operation that does not apply must be one its author was told about.
        const session = Session.empty(PROJECT);
        const noEffect = new Map<string, number>();
        for (const { operation } of sim.log) {
          if (!session.apply(operation).applied)
            noEffect.set(operation.author, (noEffect.get(operation.author) ?? 0) + 1);
        }
        sim.clients.forEach((client, i) => expect(sim.rejected[i]).toBe(noEffect.get(client.author) ?? 0));
      }),
      { numRuns: runs(200) },
    );
  });
});
