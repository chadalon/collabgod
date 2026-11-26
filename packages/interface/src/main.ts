// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interface's entry point, bundled into dist/app.js by build.mjs and served to the
// app's web view. The design system (#33) and the timeline (#31) build on this shell.

import { findJuce } from '@collabgod/bridge';
import { Shell } from './shell';
import { Surface } from './surface';

const canvas = document.getElementById('surface') as HTMLCanvasElement;
const panel = document.getElementById('diagnostics') as HTMLElement;
const surface = new Surface(canvas);
const juce = findJuce(window as { __JUCE__?: unknown });

if (juce === undefined) {
  surface.draw({ status: 'This page runs inside the CollabGod app', hostTimeMs: undefined });
} else {
  const shell = new Shell(juce.backend, surface, panel);
  window.addEventListener('error', (e) => shell.report('error', `${e.message} (${e.filename}:${e.lineno})`));
  window.addEventListener('unhandledrejection', (e) => shell.report('error', `Unhandled: ${String(e.reason)}`));
  shell.start().catch((e: unknown) => shell.report('error', `Start-up failed: ${e instanceof Error ? e.message : String(e)}`));
}
