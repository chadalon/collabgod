// SPDX-License-Identifier: GPL-3.0-or-later
//
// The programs the interface ships, checked without a web view; `CollabGod --bridge-check`
// then builds each one in the real web views in CI (#20). And the rule that keeps that
// check complete: GLSL lives only in src/shaders.ts.

import { describe, expect, it } from 'vitest';
import { checkPrograms, precisionMismatches } from '../src/glsl';
import { shaderPrograms } from '../src/shaders';
import { FakeGl } from './fakegl';

describe('the shipped shader programs', () => {
  it('have unique names', () => {
    const names = shaderPrograms.map((p) => p.name);
    expect(new Set(names).size).toBe(names.length);
  });

  it.each(shaderPrograms.map((p) => [p.name, p] as const))('%s is GLSL ES 3.00 in both stages', (_, program) => {
    expect(program.vertex.trimStart().startsWith('#version 300 es')).toBe(true);
    expect(program.fragment.trimStart().startsWith('#version 300 es')).toBe(true);
  });

  it('give every uniform used in both stages one precision, as WebKit requires', () => {
    expect(shaderPrograms.flatMap(precisionMismatches)).toEqual([]);
  });

  it('link where inputs must match outputs and WebKit is strict about precision', () => {
    expect(checkPrograms(new FakeGl({ webkit: true }).gl, shaderPrograms)).toEqual([]);
  });
});

describe('where shaders may live', () => {
  // Every file under src, whatever its extension, as text, keyed by path. A shader in a
  // .glsl file imported with ?raw must not get past these checks.
  const sources = import.meta.glob('../src/**/*', { query: '?raw', import: 'default', eager: true });

  it('reads the sources', () => {
    expect(Object.keys(sources)).toContain('../src/shaders.ts');
    expect(Object.keys(sources)).toContain('../src/glsl.ts');
  });

  it('GLSL is written only in shaders.ts (and the check’s own control programs), so the check sees every shader', () => {
    const glsl = /#version\s+\d+\s+es|\bgl_Position\b|\bgl_FragColor\b/;
    const offenders = Object.entries(sources)
      .filter(([path, text]) => glsl.test(text) && !['../src/shaders.ts', '../src/glsl.ts'].includes(path))
      .map(([path]) => path);
    expect(offenders).toEqual([]);
  });

  it('no shader is a file of its own', () => {
    const offenders = Object.keys(sources).filter((path) => /\.(glsl|vert|frag|vs|fs|wgsl)$/i.test(path));
    expect(offenders).toEqual([]);
  });

  it('only glsl.ts hands source to WebGL, so every program goes through buildProgram', () => {
    const offenders = Object.entries(sources)
      .filter(([path, text]) => /\.shaderSource\s*\(/.test(text) && path !== '../src/glsl.ts')
      .map(([path]) => path);
    expect(offenders).toEqual([]);
  });
});
