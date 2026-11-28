// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every WebGL program the interface ships, in one list. `CollabGod --bridge-check`
// compiles and links each one in the real web view, and CI runs that check in WKWebView
// on macOS and WebView2 on Windows (#20), because WebKit refused a shader that Chromium
// accepted (webview-ui finding 2). A shader written anywhere else is never checked, so
// `test/shaders.test.ts` fails if GLSL appears in any other file.
//
// Build a program from here with `buildProgram` in `glsl.ts`, the same code the check
// uses. The timeline (#31) adds the first one.

export interface ShaderProgramSource {
  /** Unique; names the program in check failures. */
  readonly name: string;
  /** GLSL ES 3.00: starts with `#version 300 es`. */
  readonly vertex: string;
  readonly fragment: string;
}

export const shaderPrograms: readonly ShaderProgramSource[] = [];
