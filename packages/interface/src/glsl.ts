// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building WebGL programs, and checking them. The interface builds every program it
// draws with through `buildProgram`, and `CollabGod --bridge-check` runs `checkPrograms`
// on every program in `shaders.ts` in the real web view, so CI compiles and links each
// one in WKWebView and in WebView2 (#20). What is checked is what ships.
//
// `precisionMismatches` checks the rule WebKit enforces and Chromium does not, without a
// web view: a uniform used in both shaders must have the same precision in both
// (webview-ui finding 2). Unit tests run it on every program.

import type { ShaderProgramSource } from './shaders';

/** The part of WebGL2 that building a program uses, so tests can pass a fake. */
export type ShaderGl = Pick<
  WebGL2RenderingContext,
  | 'VERTEX_SHADER'
  | 'FRAGMENT_SHADER'
  | 'COMPILE_STATUS'
  | 'LINK_STATUS'
  | 'createShader'
  | 'shaderSource'
  | 'compileShader'
  | 'getShaderParameter'
  | 'getShaderInfoLog'
  | 'deleteShader'
  | 'createProgram'
  | 'attachShader'
  | 'detachShader'
  | 'linkProgram'
  | 'getProgramParameter'
  | 'getProgramInfoLog'
  | 'deleteProgram'
  | 'isContextLost'
>;

export type BuildResult = { ok: true; program: WebGLProgram } | { ok: false; error: string };

/** Compiles both shaders and links them. On failure nothing is left allocated. */
export function buildProgram(gl: ShaderGl, source: ShaderProgramSource): BuildResult {
  const shaders: WebGLShader[] = [];
  const cleanUp = (): void => {
    for (const s of shaders) gl.deleteShader(s);
  };

  for (const [stage, type, text] of [
    ['vertex', gl.VERTEX_SHADER, source.vertex],
    ['fragment', gl.FRAGMENT_SHADER, source.fragment],
  ] as const) {
    const shader = gl.createShader(type);
    if (shader === null) {
      cleanUp();
      return { ok: false, error: `${source.name}: could not create the ${stage} shader${lostNote(gl)}` };
    }
    shaders.push(shader);
    gl.shaderSource(shader, text);
    gl.compileShader(shader);
    if (gl.getShaderParameter(shader, gl.COMPILE_STATUS) !== true) {
      const log = tidy(gl.getShaderInfoLog(shader));
      cleanUp();
      return { ok: false, error: `${source.name}: the ${stage} shader did not compile${log}${lostNote(gl)}` };
    }
  }

  const program = gl.createProgram();
  if (program === null) {
    cleanUp();
    return { ok: false, error: `${source.name}: could not create the program${lostNote(gl)}` };
  }
  for (const s of shaders) gl.attachShader(program, s);
  gl.linkProgram(program);
  const linked = gl.getProgramParameter(program, gl.LINK_STATUS) === true;
  const log = linked ? '' : tidy(gl.getProgramInfoLog(program));
  for (const s of shaders) gl.detachShader(program, s);
  cleanUp();
  if (!linked) {
    gl.deleteProgram(program);
    return { ok: false, error: `${source.name}: the program did not link${log}${lostNote(gl)}` };
  }
  return { ok: true, program };
}

/** Builds every program and deletes it again. One line per failure; none if all built. */
export function checkPrograms(gl: ShaderGl, programs: readonly ShaderProgramSource[]): string[] {
  const failures: string[] = [];
  const seen = new Set<string>();
  for (const source of programs) {
    if (seen.has(source.name)) failures.push(`${source.name}: two programs have this name`);
    seen.add(source.name);
    const result = buildProgram(gl, source);
    if (result.ok) gl.deleteProgram(result.program);
    else failures.push(result.error);
  }
  return failures;
}

/**
 * Two programs that tell whether the check itself works in a web view: the first must
 * build, the second must not. If the broken one builds, failures would go unseen.
 */
export const controlPrograms = {
  valid: {
    name: 'control: valid',
    vertex: `#version 300 es
uniform highp vec2 u_scale;
in vec2 a_position;
void main() { gl_Position = vec4(a_position * u_scale, 0.0, 1.0); }
`,
    fragment: `#version 300 es
precision mediump float;
uniform highp vec2 u_scale;
out vec4 colour;
void main() { colour = vec4(u_scale, 0.0, 1.0); }
`,
  },
  broken: {
    name: 'control: broken',
    vertex: `#version 300 es
in vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
`,
    fragment: `#version 300 es
precision mediump float;
out vec4 colour;
void main() { colour = vec4(1.0, 0.0, 0.0) }
`,
  },
} as const satisfies Record<string, ShaderProgramSource>;

// ---------------------------------------------------------------------------- precision

type Precision = 'highp' | 'mediump' | 'lowp' | 'unspecified';
type Kind = 'float' | 'int';

/** What a stage's precision defaults to before any `precision` statement (GLSL ES 3.00, 4.5.4). */
const builtInDefaults: Record<'vertex' | 'fragment', Record<Kind, Precision>> = {
  vertex: { float: 'highp', int: 'highp' },
  fragment: { float: 'unspecified', int: 'mediump' },
};

function kindOf(type: string): Kind | undefined {
  if (/^(float|vec[234]|mat[234](x[234])?)$/.test(type)) return 'float';
  if (/^(u?int|[iu]vec[234])$/.test(type)) return 'int';
  return undefined; // bool has no precision; samplers are left to the compiler
}

/** Without comments and preprocessor lines (`#version`), which are not statements. */
function stripComments(glsl: string): string {
  return glsl
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/\/\/[^\n]*/g, ' ')
    .replace(/^[ \t]*#[^\n]*/gm, ' ');
}

/** Each uniform's precision in one stage, by name. Uniform blocks are not looked into. */
export function uniformPrecisions(glsl: string, stage: 'vertex' | 'fragment'): Map<string, Precision> {
  const text = stripComments(glsl);
  const defaults = { ...builtInDefaults[stage] };
  const result = new Map<string, Precision>();
  // Statements in order, so a `precision` statement applies to what follows it.
  for (const statement of text.split(';')) {
    const p = /^\s*precision\s+(highp|mediump|lowp)\s+(float|int)\s*$/.exec(statement);
    if (p !== null) {
      defaults[p[2] as Kind] = p[1] as Precision;
      continue;
    }
    const u = /(?:^|[\s}])uniform\s+(?:(highp|mediump|lowp)\s+)?(\w+)\s+([\w\s,[\]]+)$/.exec(statement);
    if (u === null) continue;
    const kind = kindOf(u[2] as string);
    if (kind === undefined) continue;
    const precision = (u[1] as Precision | undefined) ?? defaults[kind];
    for (const declarator of (u[3] as string).split(',')) {
      const name = /^\s*(\w+)/.exec(declarator)?.[1];
      if (name !== undefined) result.set(name, precision);
    }
  }
  return result;
}

/** One line per uniform declared in both shaders with different precisions. */
export function precisionMismatches(source: ShaderProgramSource): string[] {
  const vertex = uniformPrecisions(source.vertex, 'vertex');
  const fragment = uniformPrecisions(source.fragment, 'fragment');
  const problems: string[] = [];
  for (const [name, v] of vertex) {
    const f = fragment.get(name);
    if (f !== undefined && f !== v) {
      problems.push(`${source.name}: uniform ${name} is ${v} in the vertex shader and ${f} in the fragment shader; WebKit refuses to link that (webview-ui finding 2)`);
    }
  }
  return problems;
}

// ---------------------------------------------------------------------------- helpers

function tidy(log: string | null): string {
  const text = (log ?? '').replace(/\0/g, '').trim().replace(/\s*\n\s*/g, ' | ');
  return text === '' ? '' : `: ${text}`;
}

function lostNote(gl: ShaderGl): string {
  return gl.isContextLost() ? ' (the WebGL context was lost)' : '';
}
