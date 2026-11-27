// SPDX-License-Identifier: GPL-3.0-or-later

import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { isPosition, positionBetween, Session, trackPositionAt } from '../src';
import { must, PROJECT, runs } from './helpers';

const key = fc
  .array(fc.constantFrom(...'0123456789abcdefghijklmnopqrstuvwxyz'), { minLength: 1, maxLength: 8 })
  .map((digits) => digits.join('').replace(/0+$/, ''))
  .filter((k) => k.length > 0);

describe('position keys', () => {
  it('a key between two others sorts strictly between them and is itself valid', () => {
    fc.assert(
      fc.property(fc.option(key, { nil: null }), fc.option(key, { nil: null }), (x, y) => {
        if (x !== null && y !== null && x === y) return;
        const [low, high] = x !== null && y !== null && x > y ? [y, x] : [x, y];
        const k = positionBetween(low, high);
        expect(isPosition(k)).toBe(true);
        if (low !== null) expect(k > low).toBe(true);
        if (high !== null) expect(k < high).toBe(true);
      }),
      { numRuns: runs(2000) },
    );
  });

  it('keeps finding room when keys are inserted again and again into the same gap', () => {
    let low: string | null = null;
    let high: string | null = null;
    for (let i = 0; i < 500; i += 1) {
      const k = positionBetween(low, high);
      expect(isPosition(k)).toBe(true);
      if (i % 2 === 0) high = k;
      else low = k;
    }
    expect((high ?? '').length).toBeLessThan(200);
  });

  it('stays short when tracks are always added at the end, or always at the start', () => {
    let last: string | null = null;
    let first: string | null = null;
    for (let i = 0; i < 1000; i += 1) {
      const nextLast = positionBetween(last, null);
      const nextFirst = positionBetween(null, first);
      if (last !== null) expect(nextLast > last).toBe(true);
      if (first !== null) expect(nextFirst < first).toBe(true);
      expect(isPosition(nextLast) && isPosition(nextFirst)).toBe(true);
      last = nextLast;
      first = nextFirst;
    }
    expect((last ?? '').length).toBeLessThanOrEqual(30);
    expect((first ?? '').length).toBeLessThanOrEqual(30);
  });

  it('refuses keys in the wrong order or of the wrong form', () => {
    expect(() => positionBetween('b', 'a')).toThrow(RangeError);
    expect(() => positionBetween('a', 'a')).toThrow(RangeError);
    expect(() => positionBetween('a0', null)).toThrow(RangeError);
    expect(() => positionBetween('', null)).toThrow(RangeError);
    expect(isPosition('a')).toBe(true);
    for (const bad of ['', 'a0', 'A', '-', 'a b', 1]) expect(isPosition(bad)).toBe(false);
  });
});

describe('trackPositionAt', () => {
  it('gives the key that puts a track at a given index', () => {
    fc.assert(
      fc.property(
        fc.array(fc.nat(20), { minLength: 1, maxLength: 30 }),
        fc.array(fc.nat(30), { maxLength: 30 }),
        (adds, moves) => {
          const session = Session.empty(PROJECT);
          adds.forEach((index, n) => {
            const id = `t${n}`;
            must(session, { type: 'track.add', track: { id, name: id, position: trackPositionAt(session, index) } });
            expect(session.trackOrder.indexOf(id)).toBe(Math.min(index, n));
          });
          moves.forEach((index, n) => {
            const id = session.trackOrder[n % session.trackOrder.length] ?? 't0';
            const position = trackPositionAt(session, index, id);
            if (position !== session.tracks.get(id)?.position)
              must(session, { type: 'track.move', trackId: id, position });
            expect(session.trackOrder.indexOf(id)).toBe(Math.min(index, session.trackOrder.length - 1));
          });
        },
      ),
      { numRuns: runs(300) },
    );
  });

  it('places a track after two that share a key, since there is no key between them', () => {
    const session = Session.empty(PROJECT);
    must(session, { type: 'track.add', track: { id: 'a', name: 'A', position: 'i' } });
    must(session, { type: 'track.add', track: { id: 'b', name: 'B', position: 'i' } });
    must(session, { type: 'track.add', track: { id: 'c', name: 'C', position: trackPositionAt(session, 1) } });
    expect(session.trackOrder).toEqual(['a', 'b', 'c']);
  });
});
