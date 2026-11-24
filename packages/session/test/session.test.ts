// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import {
  canonical,
  type Change,
  type Clip,
  DEFAULT_TEMPO,
  effectiveFades,
  MAX_CLIP_GAIN_DB,
  MIN_CLIP_GAIN_DB,
  NO_REVISION,
  type Operation,
  Session,
  TICKS_PER_QUARTER,
  ticksToSeconds,
} from '../src';
import { hash, must, newClip, op, reason, sample, SEED, unchanged } from './helpers';

/** A session with the seed: tracks t1 (Drums) and t2 (Bass), clips k1 and k2 on t1, k3 on t2. */
function seeded(): Session {
  const session = Session.empty();
  SEED.forEach((change, i) => must(session, change, 'm1', `seed:${i}`));
  return session;
}

const clip = (session: Session, id: string): Clip => {
  const found = session.clips.get(id);
  if (found === undefined) throw new Error(`no clip ${id}`);
  return found;
};

/** Apply a change, then its inverse, and check the session is exactly as it was. Returns the state in between. */
function roundTrip(session: Session, change: Change, author = 'm1'): string {
  const before = canonical(session);
  const inverse = must(session, change, author);
  const during = canonical(session);
  must(session, inverse, author);
  expect(canonical(session)).toBe(before);
  return during;
}

describe('a new session', () => {
  it('is empty, at 120 BPM in 4/4, with no revisions yet', () => {
    const session = Session.empty();
    expect(session.timeline).toEqual({
      tempo: DEFAULT_TEMPO,
      timeSignature: { numerator: 4, denominator: 4 },
      rev: { tempo: NO_REVISION, timeSignature: NO_REVISION },
    });
    expect(session.members.size + session.tracks.size + session.clips.size).toBe(0);
    expect(session.trackOrder).toEqual([]);
  });

  it('measures time in ticks, 960 to a quarter note', () => {
    expect(TICKS_PER_QUARTER).toBe(960);
    expect(ticksToSeconds(960, 120)).toBe(0.5);
    expect(ticksToSeconds(3840, 60)).toBe(4);
  });
});

describe('operations', () => {
  it('carry an id and the member who made it, and are refused without either', () => {
    const session = Session.empty();
    const change: Change = { type: 'tempo.set', tempo: 100 };
    unchanged(session, () => {
      expect(reason(session.apply({ id: '', author: 'm1', change }))).toBe('invalid');
      expect(reason(session.apply({ id: 'x', author: '', change }))).toBe('invalid');
      expect(reason(session.apply({ id: 'x', author: 'm1' } as unknown as Operation))).toBe('invalid');
      expect(reason(session.apply(null as unknown as Operation))).toBe('invalid');
    });
    expect(reason(session.apply({ id: 'x', author: 'm1', change }))).toBe('applied');
  });

  it('refuses an unknown type without changing anything', () => {
    const session = seeded();
    unchanged(session, () => {
      expect(reason(session.apply(op({ type: 'clip.explode', clipId: 'k1' } as unknown as Change)))).toBe('invalid');
    });
  });

  it('stamp every field they write with their own id', () => {
    const session = seeded();
    must(session, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 480 }, 'm2', 'move-1');
    expect(clip(session, 'k1').rev).toEqual({
      trackId: 'move-1',
      start: 'move-1',
      length: 'seed:2',
      offset: 'seed:2',
      gain: 'seed:2',
      fadeIn: 'seed:2',
      fadeOut: 'seed:2',
    });
  });
});

describe('tempo and time signature', () => {
  it('change only by operation, and each has an inverse', () => {
    const session = Session.empty();
    roundTrip(session, { type: 'tempo.set', tempo: 87.5 });
    roundTrip(session, { type: 'timeSignature.set', timeSignature: { numerator: 7, denominator: 8 } });
    must(session, { type: 'tempo.set', tempo: 140 });
    must(session, { type: 'timeSignature.set', timeSignature: { numerator: 3, denominator: 4 } });
    expect(session.timeline.tempo).toBe(140);
    expect(session.timeline.timeSignature).toEqual({ numerator: 3, denominator: 4 });
  });

  it('refuse values out of range', () => {
    const session = Session.empty();
    unchanged(session, () => {
      for (const tempo of [19.9, 999.1, Number.NaN, Number.POSITIVE_INFINITY, '120']) {
        expect(reason(session.apply(op({ type: 'tempo.set', tempo } as unknown as Change)))).toBe('invalid');
      }
      for (const timeSignature of [
        { numerator: 0, denominator: 4 },
        { numerator: 33, denominator: 4 },
        { numerator: 4, denominator: 3 },
        { numerator: 4.5, denominator: 4 },
        { numerator: 4, denominator: 4, extra: 1 },
      ]) {
        expect(reason(session.apply(op({ type: 'timeSignature.set', timeSignature } as Change)))).toBe('invalid');
      }
    });
  });

  it('report no change when the value is already set', () => {
    const session = Session.empty();
    expect(reason(session.apply(op({ type: 'tempo.set', tempo: 120 })))).toBe('no-change');
    expect(
      reason(session.apply(op({ type: 'timeSignature.set', timeSignature: { numerator: 4, denominator: 4 } }))),
    ).toBe('no-change');
  });
});

describe('members', () => {
  const add = (id: string, name = id, colour = '#ff8800'): Change => ({
    type: 'member.add',
    member: { id, name, colour },
  });

  it('are added, renamed, recoloured and marked as removed, never deleted', () => {
    const session = Session.empty();
    roundTrip(session, add('m1', 'Chad'));
    must(session, add('m1', 'Chad'));
    roundTrip(session, { type: 'member.rename', memberId: 'm1', name: 'Chad A.' });
    roundTrip(session, { type: 'member.recolour', memberId: 'm1', colour: '#00ccff' });
    roundTrip(session, { type: 'member.remove', memberId: 'm1' });
    must(session, { type: 'member.remove', memberId: 'm1' });
    expect(session.members.get('m1')).toMatchObject({ id: 'm1', name: 'Chad', colour: '#ff8800', removed: true });
    expect(reason(session.apply(op({ type: 'member.remove', memberId: 'm1' })))).toBe('no-change');
  });

  it('can be any number; nothing assumes two', () => {
    const session = Session.empty();
    for (let i = 0; i < 7; i += 1) must(session, add(`member-${i}`), `member-${i}`);
    expect(session.members.size).toBe(7);
  });

  it('refuse a taken id, an empty name and a colour that is not #rrggbb', () => {
    const session = Session.empty();
    must(session, add('m1'));
    unchanged(session, () => {
      expect(reason(session.apply(op(add('m1'))))).toBe('id-exists');
      expect(reason(session.apply(op(add('m2', '   '))))).toBe('invalid');
      expect(reason(session.apply(op(add('m2', 'x', '#FF8800'))))).toBe('invalid');
      expect(reason(session.apply(op(add('m2', 'x', 'red'))))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'member.rename', memberId: 'nobody', name: 'x' })))).toBe(
        'target-missing',
      );
    });
  });

  it('come back exactly as they were when an unconfirmed add is rolled back and redone', () => {
    const session = Session.empty();
    const inverse = must(session, add('m1', 'Chad'));
    expect(inverse.type).toBe('member.unadd');
    const redo = must(session, inverse);
    must(session, redo);
    expect(session.members.get('m1')?.name).toBe('Chad');
  });
});

describe('tracks', () => {
  it('are listed by position key, then by id', () => {
    const session = Session.empty();
    must(session, { type: 'track.add', track: { id: 'b', name: 'B', position: 'r' } });
    must(session, { type: 'track.add', track: { id: 'a', name: 'A', position: 'i' } });
    must(session, { type: 'track.add', track: { id: 'c', name: 'C', position: 'i' } });
    expect(session.trackOrder).toEqual(['a', 'c', 'b']);
  });

  it('are added, renamed and reordered, each with an inverse', () => {
    const session = seeded();
    roundTrip(session, { type: 'track.add', track: { id: 't3', name: 'Keys', position: 'm' } });
    roundTrip(session, { type: 'track.rename', trackId: 't1', name: 'Percussion' });
    const during = roundTrip(session, { type: 'track.move', trackId: 't1', position: 'z' });
    expect(during).toContain('"position":"z"');
    must(session, { type: 'track.move', trackId: 't1', position: 'z' });
    expect(session.trackOrder).toEqual(['t2', 't1']);
  });

  it('take their clips with them when removed, and the inverse brings back both', () => {
    const session = seeded();
    const during = roundTrip(session, { type: 'track.remove', trackId: 't1' });
    expect(during).not.toContain('"k1"');
    must(session, { type: 'track.remove', trackId: 't1' });
    expect([...session.clips.keys()]).toEqual(['k3']);
  });

  it('refuse a taken id, a bad position key and a missing target', () => {
    const session = seeded();
    unchanged(session, () => {
      expect(reason(session.apply(op({ type: 'track.add', track: { id: 't1', name: 'x', position: 'a' } })))).toBe(
        'id-exists',
      );
      for (const position of ['', 'a0', 'A', 'a b', 'a'.repeat(2000)]) {
        expect(reason(session.apply(op({ type: 'track.add', track: { id: 'tx', name: 'x', position } })))).toBe(
          'invalid',
        );
        expect(reason(session.apply(op({ type: 'track.move', trackId: 't1', position })))).toBe('invalid');
      }
      expect(reason(session.apply(op({ type: 'track.rename', trackId: 'gone', name: 'x' })))).toBe('target-missing');
      expect(reason(session.apply(op({ type: 'track.move', trackId: 'gone', position: 'a' })))).toBe('target-missing');
      expect(reason(session.apply(op({ type: 'track.remove', trackId: 'gone' })))).toBe('target-missing');
      expect(reason(session.apply(op({ type: 'track.rename', trackId: 't1', name: 'Drums' })))).toBe('no-change');
    });
  });

  it('removed by an inverse only while the track and its clips are exactly as the inverse expects', () => {
    const session = seeded();
    const inverse = must(session, { type: 'track.add', track: { id: 't3', name: 'Keys', position: 'm' } });
    must(session, { type: 'clip.add', clip: newClip('c9', 't3', 0, 960) }, 'm2');
    expect(reason(session.apply(op(inverse)))).toBe('guard-failed');
    expect(session.tracks.has('t3')).toBe(true);
  });
});

describe('audio clips', () => {
  it('refer to their sample by file hash, audio hash when known, and a name, never a path', () => {
    const session = seeded();
    expect(clip(session, 'k3').sample).toEqual({ fileHash: hash(3), audioHash: hash(33), name: 'sub.aif' });
    expect(clip(session, 'k1').sample).toEqual({ fileHash: hash(1), name: 'kick.wav' });
    const refused = [
      { fileHash: hash(1), name: 'C:\\Samples\\kick.wav' },
      { fileHash: hash(1), name: '/Users/chad/Samples/kick.wav' },
      { fileHash: hash(1), name: 'kick.wav', path: '/Users/chad/Samples/kick.wav' },
      { fileHash: hash(0xabc).toUpperCase(), name: 'kick.wav' },
      { fileHash: 'abc', name: 'kick.wav' },
      { fileHash: hash(1), audioHash: 'nope', name: 'kick.wav' },
      { fileHash: hash(1), name: '' },
    ];
    unchanged(session, () => {
      for (const ref of refused) {
        const change = { type: 'clip.add', clip: { ...newClip('c9', 't1', 0, 960), sample: ref } } as Change;
        expect(reason(session.apply(op(change)))).toBe('invalid');
      }
    });
  });

  it('keep only the fields they know, whatever else an operation carries', () => {
    const session = seeded();
    const change = { type: 'clip.add', clip: { ...newClip('c9', 't1', 0, 960), path: '/tmp/x.wav' } } as Change;
    must(session, change);
    expect(Object.keys(clip(session, 'c9'))).not.toContain('path');
  });

  it('are added and removed, each with an inverse', () => {
    const session = seeded();
    roundTrip(session, {
      type: 'clip.add',
      clip: newClip('c9', 't2', 480, 960, { offset: 1.5, gain: -3, fadeIn: 60 }),
    });
    roundTrip(session, { type: 'clip.remove', clipId: 'k2' });
  });

  it('move within a track and to another track', () => {
    const session = seeded();
    roundTrip(session, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 });
    roundTrip(session, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 7680 });
    must(session, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 7680 });
    expect(session.clipsOnTrack('t1').map((c) => c.id)).toEqual(['k2']);
    expect(session.clipsOnTrack('t2').map((c) => c.id)).toEqual(['k3', 'k1']);
    expect(reason(session.apply(op({ type: 'clip.move', clipId: 'k1', trackId: 't2', start: 7680 })))).toBe(
      'no-change',
    );
  });

  it('trim from the right by changing only the length', () => {
    const session = seeded();
    roundTrip(session, { type: 'clip.trim', clipId: 'k1', end: 480 });
    must(session, { type: 'clip.trim', clipId: 'k1', end: 480 }, 'm1', 'trim');
    expect(clip(session, 'k1')).toMatchObject({ start: 0, length: 480, offset: 0 });
    expect(clip(session, 'k1').rev).toMatchObject({ start: 'seed:2', length: 'trim', offset: 'seed:2' });
  });

  it('trim from the left without moving the audio: the model works out the new offset at the session’s tempo', () => {
    const session = seeded();
    roundTrip(session, { type: 'clip.trim', clipId: 'k3', start: 960 });
    must(session, { type: 'clip.trim', clipId: 'k3', start: 960 });
    expect(clip(session, 'k3')).toMatchObject({ start: 960, length: 2880, offset: 0.5 });

    must(session, { type: 'tempo.set', tempo: 90 });
    must(session, { type: 'clip.trim', clipId: 'k3', start: 1920 });
    expect(clip(session, 'k3')).toMatchObject({ start: 1920, length: 1920, offset: 0.5 + ticksToSeconds(960, 90) });
  });

  it('trim both edges at once', () => {
    const session = seeded();
    must(session, { type: 'clip.trim', clipId: 'k3', start: 480, end: 1440 });
    expect(clip(session, 'k3')).toMatchObject({ start: 480, length: 960, offset: 0.25 });
  });

  it('trim back to the very start of the file after any number of steps, despite rounding', () => {
    const session = seeded();
    must(session, { type: 'tempo.set', tempo: 87.3 });
    // Without the snap, the first two land just below zero and the last two just above it.
    for (const steps of [
      [337, 1013, 29, 2711, 5],
      [365, 2708, 127, 2916, 2158],
      [38, 213, 251],
      [38, 319, 357],
    ]) {
      for (const start of steps) must(session, { type: 'clip.trim', clipId: 'k3', start });
      must(session, { type: 'clip.trim', clipId: 'k3', start: 0 });
      expect(clip(session, 'k3')).toMatchObject({ start: 0, length: 3840, offset: 0 });
    }
  });

  it('refuse a trim that would pass the start of the file or leave no clip', () => {
    const session = seeded();
    unchanged(session, () => {
      // k2 starts at 1920 and plays its file from the beginning, so its left edge cannot go further left.
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k2', start: 960 })))).toBe('out-of-range');
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k1', start: 960 })))).toBe('out-of-range');
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k2', end: 1920 })))).toBe('out-of-range');
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k1' })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k1', end: 0 })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k1', start: 1.5 })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'k1', end: 960 })))).toBe('no-change');
    });
  });

  it('take a gain within range', () => {
    const session = seeded();
    roundTrip(session, { type: 'clip.gain', clipId: 'k1', gain: -6.5 });
    must(session, { type: 'clip.gain', clipId: 'k1', gain: MAX_CLIP_GAIN_DB });
    must(session, { type: 'clip.gain', clipId: 'k1', gain: MIN_CLIP_GAIN_DB });
    unchanged(session, () => {
      for (const gain of [MAX_CLIP_GAIN_DB + 0.1, MIN_CLIP_GAIN_DB - 1, Number.NEGATIVE_INFINITY, Number.NaN]) {
        expect(reason(session.apply(op({ type: 'clip.gain', clipId: 'k1', gain })))).toBe('invalid');
      }
    });
  });

  it('take fades in whole ticks, which sound shortened in proportion when they overlap', () => {
    const session = seeded();
    roundTrip(session, { type: 'clip.fade', clipId: 'k1', fadeIn: 120, fadeOut: 240 });
    must(session, { type: 'clip.fade', clipId: 'k1', fadeIn: 900, fadeOut: 300 });
    expect(effectiveFades(clip(session, 'k1'))).toEqual({ fadeIn: 720, fadeOut: 240 });
    expect(effectiveFades({ length: 960, fadeIn: 100, fadeOut: 200 })).toEqual({ fadeIn: 100, fadeOut: 200 });
    expect(reason(session.apply(op({ type: 'clip.fade', clipId: 'k1', fadeIn: -1, fadeOut: 0 })))).toBe('invalid');
    expect(reason(session.apply(op({ type: 'clip.fade', clipId: 'k1', fadeIn: 1.5, fadeOut: 0 })))).toBe('invalid');
  });

  it('refuse invalid spans and report a missing target or destination track', () => {
    const session = seeded();
    unchanged(session, () => {
      expect(reason(session.apply(op({ type: 'clip.add', clip: newClip('c9', 't1', -1, 960) })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.add', clip: newClip('c9', 't1', 0, 0) })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.add', clip: newClip('c9', 't1', 0.5, 960) })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.add', clip: newClip('c9', 't1', 0, 960, { offset: -0.1 }) })))).toBe(
        'invalid',
      );
      expect(reason(session.apply(op({ type: 'clip.add', clip: newClip('c9', 'gone', 0, 960) })))).toBe(
        'track-missing',
      );
      expect(reason(session.apply(op({ type: 'clip.add', clip: newClip('k1', 't1', 0, 960) })))).toBe('id-exists');
      expect(reason(session.apply(op({ type: 'clip.move', clipId: 'gone', trackId: 't1', start: 0 })))).toBe(
        'target-missing',
      );
      expect(reason(session.apply(op({ type: 'clip.move', clipId: 'k1', trackId: 'gone', start: 0 })))).toBe(
        'track-missing',
      );
      expect(reason(session.apply(op({ type: 'clip.trim', clipId: 'gone', end: 10 })))).toBe('target-missing');
      expect(reason(session.apply(op({ type: 'clip.remove', clipId: 'gone' })))).toBe('target-missing');
    });
  });
});

describe('splitting a clip', () => {
  it('cuts it in two: the left keeps the id and fade-in, the right takes the fade-out and the rest of the audio', () => {
    const session = seeded();
    must(session, { type: 'tempo.set', tempo: 90 });
    must(session, { type: 'clip.fade', clipId: 'k3', fadeIn: 60, fadeOut: 120 });
    must(session, { type: 'clip.gain', clipId: 'k3', gain: -3 });
    must(session, { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 1440 }, 'm2', 'split');

    expect(clip(session, 'k3')).toMatchObject({ start: 0, length: 1440, offset: 0, fadeIn: 60, fadeOut: 0, gain: -3 });
    expect(clip(session, 'k3b')).toMatchObject({
      trackId: 't2',
      start: 1440,
      length: 2400,
      offset: ticksToSeconds(1440, 90),
      fadeIn: 0,
      fadeOut: 120,
      gain: -3,
      sample: clip(session, 'k3').sample,
    });
    expect(clip(session, 'k3b').offset).toBe(1);
  });

  it('counts the split point from the clip’s start, so it cuts the same audio after a move', () => {
    const session = seeded();
    must(session, { type: 'clip.move', clipId: 'k3', trackId: 't1', start: 9600 });
    must(session, { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 960 });
    expect(clip(session, 'k3b')).toMatchObject({ trackId: 't1', start: 10560, offset: 0.5 });
  });

  it('has an exact inverse, and the inverse’s inverse is exact too', () => {
    const session = seeded();
    const before = canonical(session);
    const unsplit = must(session, { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 100 });
    const afterSplit = canonical(session);
    expect(unsplit.type).toBe('clip.unsplit');
    const resplit = must(session, unsplit);
    expect(canonical(session)).toBe(before);
    expect(resplit.type).toBe('clip.resplit');
    must(session, resplit);
    expect(canonical(session)).toBe(afterSplit);
  });

  it('refuses a point outside the clip, a taken id, and a missing clip', () => {
    const session = seeded();
    unchanged(session, () => {
      expect(reason(session.apply(op({ type: 'clip.split', clipId: 'k1', newClipId: 'x', at: 960 })))).toBe(
        'out-of-range',
      );
      expect(reason(session.apply(op({ type: 'clip.split', clipId: 'k1', newClipId: 'x', at: 0 })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.split', clipId: 'k1', newClipId: 'k2', at: 10 })))).toBe(
        'id-exists',
      );
      expect(reason(session.apply(op({ type: 'clip.split', clipId: 'k1', newClipId: 'k1', at: 10 })))).toBe('invalid');
      expect(reason(session.apply(op({ type: 'clip.split', clipId: 'gone', newClipId: 'x', at: 10 })))).toBe(
        'target-missing',
      );
    });
  });

  it('is not undone once someone has changed the right part', () => {
    const session = seeded();
    const unsplit = must(session, { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 100 });
    must(session, { type: 'clip.gain', clipId: 'k3b', gain: 3 }, 'm2');
    unchanged(session, () => expect(reason(session.apply(op(unsplit)))).toBe('guard-failed'));
  });
});

describe('guarded inverses', () => {
  it('apply only while each field they write was last written by the edit they reverse', () => {
    const session = seeded();
    const undoMove = must(session, { type: 'clip.move', clipId: 'k1', trackId: 't1', start: 480 }, 'm1');
    must(session, { type: 'clip.trim', clipId: 'k1', start: 600 }, 'm2');
    // The other person rewrote `start`, so the undo of the move must not touch it.
    unchanged(session, () => expect(reason(session.apply(op(undoMove)))).toBe('guard-failed'));
  });

  it('work field by field: undoing a move keeps someone else’s change to the gain', () => {
    const session = seeded();
    const undoMove = must(session, { type: 'clip.move', clipId: 'k1', trackId: 't2', start: 480 }, 'm1');
    must(session, { type: 'clip.gain', clipId: 'k1', gain: -12 }, 'm2');
    must(session, undoMove);
    expect(clip(session, 'k1')).toMatchObject({ trackId: 't1', start: 0, gain: -12 });
  });

  it('report a missing target as rejected, not as an error, and change nothing', () => {
    const session = seeded();
    const undoGain = must(session, { type: 'clip.gain', clipId: 'k1', gain: 3 });
    must(session, { type: 'track.remove', trackId: 't1' }, 'm2');
    unchanged(session, () => expect(reason(session.apply(op(undoGain)))).toBe('target-missing'));
  });

  it('refuse a malformed patch', () => {
    const session = seeded();
    const bad: unknown[] = [
      { type: 'clip.patch', clipId: 'k1', set: {}, expect: {}, restore: {} },
      { type: 'clip.patch', clipId: 'k1', set: { start: 1 }, expect: {}, restore: { start: 'x' } },
      { type: 'clip.patch', clipId: 'k1', set: { start: -1 }, expect: { start: 'seed:2' }, restore: { start: 'x' } },
      {
        type: 'clip.patch',
        clipId: 'k1',
        set: { sample: sample(9) },
        expect: { sample: 'x' },
        restore: { sample: 'x' },
      },
      {
        type: 'track.patch',
        trackId: 't1',
        set: { name: 'x' },
        expect: { name: 'seed:0', position: 'x' },
        restore: { name: 'x' },
      },
      { type: 'timeline.patch', set: { tempo: 5 }, expect: { tempo: '' }, restore: { tempo: '' } },
    ];
    unchanged(session, () => {
      for (const change of bad) expect(reason(session.apply(op(change as Change)))).toBe('invalid');
    });
  });
});

describe('snapshots', () => {
  it('round-trip exactly, revisions included', () => {
    const session = seeded();
    must(session, { type: 'member.add', member: { id: 'm1', name: 'Chad', colour: '#112233' } });
    must(session, { type: 'tempo.set', tempo: 96 });
    must(session, { type: 'clip.split', clipId: 'k3', newClipId: 'k3b', at: 333 });
    const copy = Session.fromSnapshot(session.snapshot());
    expect(canonical(copy)).toBe(canonical(session));
    expect(copy.checkInvariants()).toEqual([]);
  });

  it('do not depend on the order things were added in', () => {
    const a = seeded();
    const b = Session.empty();
    [...SEED].reverse().forEach((change) => b.apply(op(change)));
    // Clips came before their tracks in b, so b holds only the tracks. Add the clips again in a third order.
    for (const change of [SEED[4], SEED[2], SEED[3]]) if (change) b.apply(op(change));
    const strip = (s: Session) => canonical(s).replace(/"(rev)":\{[^}]*\}/g, '');
    expect(strip(b)).toBe(strip(a));
  });

  it('refuse a clip on a missing track, a path in a sample, and duplicates', () => {
    const snapshot = seeded().snapshot();
    const k1 = snapshot.clips[0];
    if (k1 === undefined) throw new Error('seed has clips');
    expect(() => Session.fromSnapshot({ ...snapshot, clips: [{ ...k1, trackId: 'gone' }] })).toThrow(/missing track/);
    expect(() =>
      Session.fromSnapshot({ ...snapshot, clips: [{ ...k1, sample: { ...k1.sample, path: '/x.wav' } as never }] }),
    ).toThrow(/clip/);
    expect(() => Session.fromSnapshot({ ...snapshot, clips: [k1, k1] })).toThrow(/twice/);
  });
});
