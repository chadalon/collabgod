// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { ContractError, decode, namedTypes, sameShape, t } from '../src/schema';

const Point = t.object('Point', { x: t.number(), y: t.number() });
const Shape = t.object('Shape', {
  name: t.string(),
  visible: t.boolean(),
  layer: t.integer(),
  kind: t.enum('Kind', ['line', 'fill']),
  points: t.array(Point),
});

const shape = { name: 'a', visible: true, layer: 2, kind: 'line', points: [{ x: 0, y: 1.5 }] };

describe('decode', () => {
  it('returns a matching value, typed, as a copy', () => {
    const value = decode(Shape, shape);
    expect(value).toEqual(shape);
    expect(value).not.toBe(shape);
    expect(value.points[0]).not.toBe(shape.points[0]);
  });

  const { name: _name, ...withoutName } = shape;

  it.each([
    ['a missing field', withoutName, 'value.name is missing'],
    ['a field it does not know', { ...shape, colour: 'red' }, 'value.colour is not a field of Shape'],
    ['a string for a number', { ...shape, points: [{ x: '0', y: 1 }] }, 'value.points[0].x is a string, not a finite number'],
    ['NaN, which JSON cannot carry but a bug can make', { ...shape, points: [{ x: Number.NaN, y: 1 }] }, 'value.points[0].x is NaN, not a finite number'],
    ['infinity', { ...shape, points: [{ x: 0, y: Infinity }] }, 'value.points[0].y is Infinity, not a finite number'],
    ['a fraction for an integer', { ...shape, layer: 2.5 }, 'value.layer is 2.5, not a whole number'],
    ['an integer past the safe range', { ...shape, layer: 2 ** 53 }, `value.layer is ${2 ** 53}, not a whole number`],
    ['a value outside the enum', { ...shape, kind: 'dots' }, 'value.kind is "dots", not one of line, fill'],
    ['an object for an array', { ...shape, points: {} }, 'value.points is an object, not an array'],
    ['null for an object', null, 'value is null, not an object (Shape)'],
    ['an array for an object', [], 'value is an array, not an object (Shape)'],
  ])('refuses %s, naming where', (_, value, message) => {
    expect(() => decode(Shape, value)).toThrow(ContractError);
    expect(() => decode(Shape, value)).toThrow(message);
  });

  it('reports the path and the problem separately', () => {
    try {
      decode(Shape, { ...shape, layer: 'x' }, 'shape');
      expect.unreachable();
    } catch (e) {
      expect(e).toBeInstanceOf(ContractError);
      expect((e as ContractError).path).toBe('shape.layer');
      expect((e as ContractError).problem).toBe('is a string, not a whole number');
    }
  });

  it('does not take inherited fields as present', () => {
    const value = Object.create({ x: 1 }) as Record<string, unknown>;
    value['y'] = 2;
    expect(() => decode(Point, value)).toThrow('value.x is missing');
  });
});

describe('builders', () => {
  it('refuse names C++ cannot use', () => {
    expect(() => t.object('my-type', {})).toThrow();
    expect(() => t.object('Ok', { 'bad field': t.number() })).toThrow();
    expect(() => t.enum('E', [])).toThrow('has no values');
    expect(() => t.enum('E', ['a', 'a'])).toThrow('repeats');
    expect(() => t.enum('E', ['1st'])).toThrow();
  });

  it('carry doc comments without changing the shape', () => {
    const documented = t.doc(Point, 'Where it is');
    expect(documented.doc).toBe('Where it is');
    expect(sameShape(documented, Point)).toBe(true);
  });
});

describe('namedTypes', () => {
  it('lists every object and enum once, dependencies first', () => {
    const names = namedTypes([Shape, Point]).map((s) => s.name);
    expect(names).toEqual(['Kind', 'Point', 'Shape']);
  });

  it('refuses two different types with one name', () => {
    const other = t.object('Point', { x: t.number() });
    expect(() => namedTypes([Shape, other])).toThrow('Two different types are both named Point');
  });
});
