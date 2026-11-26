// SPDX-License-Identifier: GPL-3.0-or-later

import { canonical, type Change, type Operation, opIdSource, Replica, Sequencer, Session } from '@collabgod/session';
import { type LoadResult, type Project, projectOf } from '../src';

export { runs } from '../../session/test/helpers';

/** A well-formed SHA-256 text for test number `n`. */
export const hash = (n: number): string => n.toString(16).padStart(64, '0');

/** One person working alone, as the app does before any lobby: every edit is ordered at once. */
export function solo(author = 'chad', run = 'run1') {
  const me = new Replica({ author, nextOpId: opIdSource(`${author}:${run}:`) });
  const order = new Sequencer();
  const send = (operations: readonly Operation[]): void => {
    for (const operation of operations) send(me.receive(order.next(operation)).send);
  };
  const edit = (change: Change): void => {
    const result = me.submit(change);
    if (!result.applied) throw new Error(`${change.type} did not apply: ${result.reason}`);
    send(result.send);
  };
  return { me, edit, send };
}

/** A small project: a member, two tracks, three clips (one with an audio hash), a few edits and an undo. */
export function smallProject(): Project {
  const { me, edit, send } = solo();
  edit({
    type: 'member.add',
    member: { id: 'chad', name: 'Chad', colour: '#ff8800', verifier: { scheme: 'sha256', value: hash(0x5ec) } },
  });
  edit({ type: 'tempo.set', tempo: 92.5 });
  edit({ type: 'timeSignature.set', timeSignature: { numerator: 7, denominator: 8 } });
  edit({ type: 'track.add', track: { id: 't1', name: 'Drums', position: 'i' } });
  edit({ type: 'track.add', track: { id: 't2', name: 'Bass', position: 'r' } });
  const clip = (id: string, trackId: string, start: number, length: number, n: number, name: string) =>
    edit({
      type: 'clip.add',
      clip: {
        id,
        trackId,
        start,
        length,
        offset: 0,
        gain: 0,
        fadeIn: 0,
        fadeOut: 0,
        sample: n === 3 ? { fileHash: hash(n), audioHash: hash(33), name } : { fileHash: hash(n), name },
      },
    });
  clip('k1', 't1', 0, 960, 1, 'kick.wav');
  clip('k2', 't1', 1920, 960, 2, 'snare.wav');
  clip('k3', 't2', 0, 3840, 3, 'sub.aif');
  edit({ type: 'clip.trim', clipId: 'k3', from: 0.25 });
  edit({ type: 'clip.gain', clipId: 'k2', gain: -4.5 });
  const undone = me.undo();
  if (undone.applied) send(undone.send);
  return projectOf(me);
}

export function loaded(result: LoadResult): Project {
  if (!result.ok) throw new Error(`did not load: ${result.error.kind}: ${result.error.detail}`);
  return result.project;
}

/** The model state a project holds, as text: the session with revisions and authors, its seq, and the history. */
export function stateOf(project: Project): string {
  return JSON.stringify({
    session: canonical(Session.fromSnapshot(project.session)),
    seq: project.seq,
    history: project.history ?? null,
  });
}
