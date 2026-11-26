// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { buildProgram, checkPrograms, controlPrograms, precisionMismatches, uniformPrecisions } from '../src/glsl';
import type { ShaderProgramSource } from '../src/shaders';
import { FakeGl } from './fakegl';

/** The pair from webview-ui finding 2: Chromium linked it, WebKit refused. */
const findingTwo: ShaderProgramSource = {
  name: 'finding 2',
  vertex: `#version 300 es
uniform vec4 u_world;
in vec2 a_position;
void main() { gl_Position = vec4(a_position * u_world.zw + u_world.xy, 0.0, 1.0); }
`,
  fragment: `#version 300 es
precision mediump float;
uniform vec4 u_world;
out vec4 colour;
void main() { colour = u_world; }
`,
};

describe('buildProgram', () => {
  it('builds a valid program and leaves only the program allocated', () => {
    const fake = new FakeGl({ webkit: true });
    const result = buildProgram(fake.gl, controlPrograms.valid);
    expect(result.ok).toBe(true);
    expect(fake.liveShaders.size).toBe(0);
    expect(fake.livePrograms.size).toBe(1);
  });

  it('says which shader did not compile, with the compiler log on one line, and leaks nothing', () => {
    const fake = new FakeGl();
    const result = buildProgram(fake.gl, controlPrograms.broken);
    expect(result).toEqual({ ok: false, error: "control: broken: the fragment shader did not compile: ERROR: 0:4: '}' : syntax error" });
    expect(fake.liveShaders.size).toBe(0);
    expect(fake.livePrograms.size).toBe(0);
  });

  it('says when the program did not link, and leaks nothing', () => {
    const fake = new FakeGl({ webkit: true });
    const result = buildProgram(fake.gl, findingTwo);
    expect(result.ok).toBe(false);
    expect(result.ok ? '' : result.error).toBe('finding 2: the program did not link: Precisions of uniform differ between VERTEX and FRAGMENT shaders');
    expect(fake.liveShaders.size).toBe(0);
    expect(fake.livePrograms.size).toBe(0);
  });

  it('links that same program where Chromium would, which is why both web views are checked', () => {
    expect(buildProgram(new FakeGl().gl, findingTwo).ok).toBe(true);
  });

  it('says when the context is lost', () => {
    const result = buildProgram(new FakeGl({ lost: true }).gl, controlPrograms.valid);
    expect(result).toEqual({ ok: false, error: 'control: valid: could not create the vertex shader (the WebGL context was lost)' });
  });
});

describe('checkPrograms', () => {
  it('builds and deletes every program, and reports nothing when all build', () => {
    const fake = new FakeGl({ webkit: true });
    const second = { ...controlPrograms.valid, name: 'second' };
    expect(checkPrograms(fake.gl, [controlPrograms.valid, second])).toEqual([]);
    expect(fake.linked).toHaveLength(2);
    expect(fake.livePrograms.size).toBe(0);
  });

  it('reports every failure, not only the first', () => {
    const fake = new FakeGl({ webkit: true });
    const failures = checkPrograms(fake.gl, [controlPrograms.broken, findingTwo, controlPrograms.valid]);
    expect(failures).toHaveLength(2);
    expect(failures[0]).toContain('control: broken');
    expect(failures[1]).toContain('finding 2');
  });

  it('reports two programs with one name', () => {
    const failures = checkPrograms(new FakeGl().gl, [controlPrograms.valid, controlPrograms.valid]);
    expect(failures).toEqual(['control: valid: two programs have this name']);
  });
});

describe('the control programs', () => {
  it('are one that builds and one that does not', () => {
    const fake = new FakeGl({ webkit: true });
    expect(buildProgram(fake.gl, controlPrograms.valid).ok).toBe(true);
    expect(buildProgram(fake.gl, controlPrograms.broken).ok).toBe(false);
  });

  it('break the broken one only by its missing semicolon', () => {
    const { broken } = controlPrograms;
    const fixed = { ...broken, fragment: broken.fragment.replace('0.0) }', '0.0); }') };
    expect(fixed.fragment).not.toBe(broken.fragment);
    expect(buildProgram(new FakeGl({ webkit: true }).gl, fixed).ok).toBe(true);
  });
});

describe('uniformPrecisions', () => {
  it('uses each stage’s built-in defaults: highp in the vertex shader, none for float in the fragment shader', () => {
    expect(uniformPrecisions('uniform vec2 a; uniform int b;', 'vertex')).toEqual(
      new Map([
        ['a', 'highp'],
        ['b', 'highp'],
      ]),
    );
    expect(uniformPrecisions('uniform vec2 a; uniform int b;', 'fragment')).toEqual(
      new Map([
        ['a', 'unspecified'],
        ['b', 'mediump'],
      ]),
    );
  });

  it('applies a precision statement to what follows it, and an explicit qualifier over both', () => {
    const glsl = `#version 300 es
uniform float before;
precision lowp float;
uniform float after;
uniform highp mat3 explicit;`;
    expect(uniformPrecisions(glsl, 'fragment')).toEqual(
      new Map([
        ['before', 'unspecified'],
        ['after', 'lowp'],
        ['explicit', 'highp'],
      ]),
    );
  });

  it('reads several names in one declaration and arrays, and ignores comments, samplers, bools and blocks', () => {
    const glsl = `
// uniform float commented;
/* uniform float alsoCommented; */
uniform mediump vec4 a, b[4];
uniform sampler2D s;
uniform bool flag;
uniform Block { highp vec4 inBlock; };`;
    expect(uniformPrecisions(glsl, 'vertex')).toEqual(
      new Map([
        ['a', 'mediump'],
        ['b', 'mediump'],
      ]),
    );
  });
});

describe('precisionMismatches', () => {
  it('finds the uniform from webview-ui finding 2', () => {
    expect(precisionMismatches(findingTwo)).toEqual([
      'finding 2: uniform u_world is highp in the vertex shader and mediump in the fragment shader; WebKit refuses to link that (webview-ui finding 2)',
    ]);
  });

  it('accepts the fix: highp in both', () => {
    const fixed = { ...findingTwo, fragment: findingTwo.fragment.replace('uniform vec4', 'uniform highp vec4') };
    expect(precisionMismatches(fixed)).toEqual([]);
  });

  it('ignores uniforms used in one shader only', () => {
    expect(precisionMismatches(controlPrograms.broken)).toEqual([]);
  });
});
