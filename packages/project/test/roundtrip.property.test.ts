// SPDX-License-Identifier: GPL-3.0-or-later

import { canonical, type Operation, opIdSource, type Replica, Sequencer } from '@collabgod/session';
import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { type Dice, dice, idSource, randomChange, run, Sim, stepOf } from '../../session/test/sim';
import { loadProject, type Project, projectOf, replicaFor, saveProject } from '../src';
import { loaded, runs, stateOf } from './helpers';

/** Save a project as text and open it again. */
const reopen = (project: Project): Project => loaded(loadProject(saveProject(project)));

type SoloStep = { kind: 'edit'; dice: Dice } | { kind: 'undo' } | { kind: 'redo' };

const soloStep = fc.oneof(
  { weight: 3, arbitrary: fc.record({ kind: fc.constant('edit' as const), dice }) },
  { weight: 2, arbitrary: fc.record({ kind: fc.constant('undo' as const) }) },
  { weight: 1, arbitrary: fc.record({ kind: fc.constant('redo' as const) }) },
) as fc.Arbitrary<SoloStep>;

const soloSteps = (minLength: number, maxLength: number) => fc.array(soloStep, { minLength, maxLength });

/** Long enough lobby runs that every member has confirmed undo history to save. */
const lobbySteps = fc.array(stepOf(), { minLength: 40, maxLength: 160 });

/** Work alone on a replica: each step is ordered at once, as with no relay. Returns what each step did. */
function workOn(me: Replica, steps: readonly SoloStep[]): string[] {
  const order = new Sequencer(me.seq);
  const ids = idSource(`${me.author}-new`);
  const send = (operations: readonly Operation[]): void => {
    for (const operation of operations) send(me.receive(order.next(operation)).send);
  };
  return steps.map((step) => {
    let outcome: unknown;
    if (step.kind === 'edit') {
      const change = randomChange(me.view, step.dice, ids);
      const result = change === undefined ? undefined : me.submit(change);
      if (result?.applied) send(result.send);
      outcome = result?.applied ?? null;
    } else {
      const result = step.kind === 'undo' ? me.undo() : me.redo();
      if (result.applied) send(result.send);
      outcome = result.applied ? [result.reverses, result.skipped] : result.skipped;
    }
    return JSON.stringify([outcome, canonical(me.view)]);
  });
}

describe('save then load', () => {
  it('reproduces exactly the model state that was saved, and the same text when saved again, in a lobby', () => {
    fc.assert(
      fc.property(lobbySteps, fc.boolean(), (steps, quiet) => {
        // With edits still in flight, or once the network is quiet: either way what is saved is each
        // replica's confirmed state and its member's confirmed history.
        const sim = run(3, steps);
        if (quiet) sim.drain();
        for (const client of sim.clients) {
          const project = projectOf(client);
          const text = saveProject(project);
          const back = loaded(loadProject(text));
          expect(stateOf(back)).toBe(stateOf(project));
          expect(saveProject(back)).toBe(text);
        }
      }),
      { numRuns: runs(200) },
    );
  });

  it('reopens a lobby member’s project as a replica that behaves exactly as one not saved to a file', () => {
    fc.assert(
      fc.property(lobbySteps, fc.nat(2), soloSteps(0, 40), (steps, who, later) => {
        const sim = run(3, steps);
        sim.drain();
        const client = sim.client(who);
        const project = projectOf(client);
        const options = () => ({ author: client.author, nextOpId: opIdSource(`${client.author}:reopened:`) });
        const direct = replicaFor(project, options());
        const fromFile = replicaFor(reopen(project), options());
        expect(canonical(fromFile.view)).toBe(canonical(client.view));
        expect(workOn(fromFile, later)).toEqual(workOn(direct, later));
      }),
      { numRuns: runs(200) },
    );
  });

  it('does the same for someone working alone, with long undo and redo histories', () => {
    fc.assert(
      fc.property(soloSteps(20, 120), soloSteps(0, 40), (before, after) => {
        const me = new Sim(1).client(0);
        workOn(me, before);
        const project = projectOf(me);
        const text = saveProject(project);
        const back = loaded(loadProject(text));
        expect(stateOf(back)).toBe(stateOf(project));
        expect(saveProject(back)).toBe(text);
        const options = () => ({ author: me.author, nextOpId: opIdSource(`${me.author}:reopened:`) });
        expect(workOn(replicaFor(back, options()), after)).toEqual(workOn(replicaFor(project, options()), after));
      }),
      { numRuns: runs(200) },
    );
  });
});
