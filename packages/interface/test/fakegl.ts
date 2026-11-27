// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for WebGL2's shader calls, so building and checking programs can be tested
// without a web view. It refuses what a real compiler would in the cases the tests use,
// and counts what is still allocated, to check that nothing leaks.

import { precisionMismatches, type ShaderGl } from '../src/glsl';

export interface FakeGlOptions {
  /** Refuse to link a uniform's precisions that differ, as WebKit does (webview-ui finding 2). Chromium does not. */
  webkit?: boolean;
  /** Whether a shader compiles. By default: not if a statement lacks its semicolon before `}`. */
  compiles?: (source: string) => boolean;
  /** No context, as when it is lost: every create call returns null. */
  lost?: boolean;
  /** Link anything that compiled, as no real web view does. */
  linksEverything?: boolean;
}

interface Shader {
  type: number;
  source: string;
  compiled: boolean;
}

interface Program {
  shaders: Shader[];
  linked: boolean;
  log: string;
}

export class FakeGl {
  readonly VERTEX_SHADER = 0x8b31;
  readonly FRAGMENT_SHADER = 0x8b30;
  readonly COMPILE_STATUS = 0x8b81;
  readonly LINK_STATUS = 0x8b82;

  readonly liveShaders = new Set<Shader>();
  readonly livePrograms = new Set<Program>();
  /** Every program linked successfully, in order. */
  readonly linked: Program[] = [];

  constructor(private readonly options: FakeGlOptions = {}) {}

  /** This fake, typed as the part of WebGL2 the code under test uses. */
  get gl(): ShaderGl {
    return this as unknown as ShaderGl;
  }

  createShader(type: number): Shader | null {
    if (this.options.lost) return null;
    const shader = { type, source: '', compiled: false };
    this.liveShaders.add(shader);
    return shader;
  }

  shaderSource(shader: Shader, source: string): void {
    shader.source = source;
  }

  compileShader(shader: Shader): void {
    const compiles = this.options.compiles ?? ((s) => !/\)\s*\}/.test(s));
    shader.compiled = compiles(shader.source);
  }

  getShaderParameter(shader: Shader, pname: number): boolean {
    expectParameter(pname, this.COMPILE_STATUS);
    return shader.compiled;
  }

  getShaderInfoLog(shader: Shader): string {
    return shader.compiled ? '' : "ERROR: 0:4: '}' : syntax error\n\0";
  }

  deleteShader(shader: Shader): void {
    this.liveShaders.delete(shader);
  }

  createProgram(): Program | null {
    if (this.options.lost) return null;
    const program = { shaders: [], linked: false, log: '' };
    this.livePrograms.add(program);
    return program;
  }

  attachShader(program: Program, shader: Shader): void {
    program.shaders.push(shader);
  }

  detachShader(program: Program, shader: Shader): void {
    program.shaders = program.shaders.filter((s) => s !== shader);
  }

  linkProgram(program: Program): void {
    const vertex = program.shaders.find((s) => s.type === this.VERTEX_SHADER);
    const fragment = program.shaders.find((s) => s.type === this.FRAGMENT_SHADER);
    if (vertex === undefined || fragment === undefined) {
      program.log = 'missing a shader';
      return;
    }
    if (this.options.linksEverything) {
      program.linked = true;
      this.linked.push(program);
      return;
    }
    // Every input the fragment shader declares needs a matching vertex output, in every engine.
    const outs = new Set(declared(vertex.source, 'out'));
    const missing = declared(fragment.source, 'in').filter((name) => !outs.has(name));
    if (missing.length > 0) {
      program.log = `Input of fragment shader '${missing[0]}' not written by vertex shader`;
      return;
    }
    const mismatches = precisionMismatches({ name: 'fake', vertex: vertex.source, fragment: fragment.source });
    if (this.options.webkit && mismatches.length > 0) {
      program.log = 'Precisions of uniform differ between VERTEX and FRAGMENT shaders';
      return;
    }
    program.linked = true;
    this.linked.push(program);
  }

  getProgramParameter(program: Program, pname: number): boolean {
    expectParameter(pname, this.LINK_STATUS);
    return program.linked;
  }

  getProgramInfoLog(program: Program): string {
    return program.log;
  }

  deleteProgram(program: Program): void {
    this.livePrograms.delete(program);
  }

  isContextLost(): boolean {
    return this.options.lost ?? false;
  }
}

/** Names declared `in` or `out` at the top level of a shader. */
function declared(source: string, qualifier: 'in' | 'out'): string[] {
  const pattern = new RegExp(`\\b${qualifier}\\s+(?:(?:highp|mediump|lowp|flat|smooth)\\s+)*\\w+\\s+(\\w+)\\s*;`, 'g');
  return [...source.matchAll(pattern)].map((m) => m[1] as string);
}

function expectParameter(actual: number, expected: number): void {
  if (actual !== expected) throw new Error(`the fake only answers parameter ${expected}, not ${actual}`);
}
