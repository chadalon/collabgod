// SPDX-License-Identifier: GPL-3.0-or-later
//
// The page's half of `CollabGod --bridge-check`: every function called, every event
// received and every command sent through the real web view, each answer checked
// against the contract. The app checks its half (every message arrived) when the report
// comes in. Loops over the contract, so a new message must be exercised here or the
// check fails and says which.

import {
  backgroundColour,
  type BridgeClient,
  type CommandName,
  type CommandQueue,
  commands,
  contractVersion,
  type EventName,
  events,
  type FunctionName,
  functions,
  type Infer,
  type SelfCheckReport,
} from '@collabgod/bridge';
import type { Diagnostics, HostInfo, ProbeTracker } from './diagnostics';
import type { FrameDelivery, FrameFeed } from './feed';
import { buildProgram, checkPrograms, controlPrograms, type ShaderGl } from './glsl';
import { type ShaderProgramSource, shaderPrograms } from './shaders';

export interface SelfCheckDeps {
  client: BridgeClient;
  queue: CommandQueue;
  feed: FrameFeed;
  probes: ProbeTracker;
  host: HostInfo;
  /** Events the page has received from the app, by name. */
  eventsSeen: ReadonlySet<EventName>;
  /** Frames received through the `frame` event. */
  pushedFrames: () => number;
  /** The computed background colours of the page's root and body, as CSS. */
  backgrounds: () => string[];
  diagnostics: () => Diagnostics;
  /** audioChanged events received so far. */
  audioEvents: () => number;
  /** Changes delivery on both sides. */
  setDelivery: (d: FrameDelivery) => Promise<void>;
  /** Resolves once `ready` returns true, polling every few milliseconds; rejects after `timeoutMs`. */
  waitFor: (ready: () => boolean, timeoutMs: number) => Promise<void>;
  now: () => number;
  /** Adds a ping's round trip to the diagnostics. */
  recordRoundTrip: (ms: number) => void;
  /** How long to wait for frames or a command to show; 5 s by default. */
  stepTimeoutMs?: number;
  /** A WebGL2 context to check the shaders in, and how to give it back; undefined if there is none. */
  webgl: () => { gl: ShaderGl; renderer: string; release: () => void } | undefined;
  /** The programs to check; every program the interface ships by default. */
  shaderPrograms?: readonly ShaderProgramSource[];
}

export type Report = Infer<typeof SelfCheckReport>;

/** `#0b0d12` as a browser reports it computed: `rgb(11, 13, 18)`. */
export function cssRgb(hex: string): string {
  const n = Number.parseInt(hex.slice(1), 16);
  return `rgb(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255})`;
}

export async function runSelfCheck(deps: SelfCheckDeps): Promise<Report> {
  const { client, queue, feed, probes } = deps;
  const failures: string[] = [];
  let checks = 0;
  const called = new Set<FunctionName>();
  const sent = new Set<CommandName>();

  const check = (ok: boolean, what: string): void => {
    checks += 1;
    if (!ok) failures.push(what);
  };

  /** Runs one step; a step that throws (a refused or mistyped answer, a time-out) is a failure. */
  const step = async (what: string, fn: () => Promise<void>): Promise<void> => {
    try {
      await fn();
    } catch (e) {
      check(false, `${what}: ${e instanceof Error ? e.message : String(e)}`);
    }
  };

  const call: BridgeClient['call'] = (name, ...params) => {
    called.add(name);
    return client.call(name, ...params);
  };

  const original = feed.frameDelivery;
  const timeout = deps.stepTimeoutMs ?? 5000;

  await step('hello', async () => {
    const info = await call('hello');
    check(info.contractVersion === contractVersion, `hello: the app speaks contract v${info.contractVersion}, the page v${contractVersion}`);
    check(info.selfCheck, 'hello: the app did not ask for a self-check');
  });

  await step('ping', async () => {
    for (let i = 0; i < 20; i++) {
      const sentAtMs = deps.now();
      const answer = await call('ping', { sentAtMs });
      deps.recordRoundTrip(deps.now() - sentAtMs);
      check(answer.sentAtMs === sentAtMs, 'ping: the app did not return sentAtMs unchanged');
    }
  });

  await step('log', async () => {
    await call('log', { level: 'info', message: 'Bridge self-check: every function, event and command' });
  });

  await step('pullFrame', async () => {
    const a = await call('pullFrame');
    const b = await call('pullFrame');
    check(b.seq > a.seq && b.hostTimeMs >= a.hostTimeMs, `pullFrame: frames did not count up (${a.seq}, then ${b.seq})`);
  });

  await step('push delivery', async () => {
    called.add('setFrameDelivery');
    await deps.setDelivery('push');
    const before = deps.pushedFrames();
    await deps.waitFor(() => deps.pushedFrames() >= before + 3, timeout);
    check(true, 'frames are pushed');
  });

  await step('commands', async () => {
    const batches = queue.batches;
    let last = -1;
    for (let i = 0; i < 3; i++) {
      last = probes.next(deps.now());
      queue.push('probe', { id: last });
      sent.add('probe');
    }
    await deps.waitFor(() => (feed.latest?.lastProbe ?? -1) >= last, timeout);
    check(queue.batches === batches + 1, `three commands queued together went in ${queue.batches - batches} batches, not 1`);
  });

  await step('pull delivery', async () => {
    await deps.setDelivery('pull');
    const before = feed.received;
    await deps.waitFor(() => feed.received >= before + 3, timeout);
    check(true, 'frames are pulled');
  });

  await step('restore delivery', () => deps.setDelivery(original));

  await step('setFrameDelivery answer', async () => {
    const answer = await call('setFrameDelivery', { frameDelivery: original });
    check(answer.frameDelivery === original, `setFrameDelivery: asked for ${original}, the app uses ${answer.frameDelivery}`);
  });

  await step('reportDiagnostics', () => call('reportDiagnostics', deps.diagnostics()));

  // The audio settings (#27). `--bridge-check` runs the app on simulated sound cards with
  // settings in a temporary folder, so this changes nothing a person chose. It opens another
  // device of the open type, waits for audioChanged, and puts the original choice back.
  await step('audio output', async () => {
    const devices = await call('audioDevices');
    check(devices.types.length > 0, 'audioDevices: the app lists no driver types');
    const before = devices.status;
    check(before.state !== 'none', `audioDevices: no output is open (${before.message})`);
    const other = devices.types.find((t) => t.name === before.type)?.devices.find((d) => d !== before.device);
    if (other === undefined) {
      check(false, `chooseAudioOutput: needs a second device of type ${before.type} to switch to; the app runs this on simulated ones`);
      return;
    }

    const events = deps.audioEvents();
    const switched = await call('chooseAudioOutput', { ...before.chosen, type: before.type, device: other, sampleRate: 0, bufferSize: 0 });
    check(switched.state === 'open' && switched.device === other, `chooseAudioOutput: asked for ${other}, the app has ${switched.device} (${switched.message})`);
    await deps.waitFor(() => deps.audioEvents() > events, timeout);

    const back = await call('chooseAudioOutput', before.chosen);
    check(back.device === before.device, `chooseAudioOutput: put back ${before.device}, the app has ${back.device} (${back.message})`);
    const frame = await call('pullFrame');
    check(frame.lateCallbacks >= 0, 'pullFrame: lateCallbacks is negative');
  });

  // Every shader the interface ships, compiled and linked in this web view (#20). CI runs
  // this in WKWebView and WebView2, because WebKit refused one that Chromium accepted.
  await step('shaders', async () => {
    const context = deps.webgl();
    if (context === undefined) {
      check(false, 'shaders: no WebGL2 context, and the interface draws with WebGL2 (webview-ui finding 3)');
      return;
    }
    const { gl } = context;
    try {
      const valid = buildProgram(gl, controlPrograms.valid);
      check(valid.ok, `shaders: a valid program did not build, so no shader can be checked here (${valid.ok ? '' : valid.error})`);
      if (valid.ok) gl.deleteProgram(valid.program);
      const broken = buildProgram(gl, controlPrograms.broken);
      check(!broken.ok, 'shaders: a program with a syntax error built, so this check would miss a broken shader');
      if (broken.ok) gl.deleteProgram(broken.program);
      const unlinked = buildProgram(gl, controlPrograms.unlinked);
      check(
        !unlinked.ok && unlinked.error.includes('did not link'),
        `shaders: a program whose shaders do not match ${unlinked.ok ? 'linked' : `failed before linking (${unlinked.error})`}, so this check would miss a link failure`,
      );
      if (unlinked.ok) gl.deleteProgram(unlinked.program);

      const programs = deps.shaderPrograms ?? shaderPrograms;
      const failures = checkPrograms(gl, programs);
      for (const f of failures) check(false, `shaders: ${f}`);
      await call('log', { level: 'info', message: `Shaders: ${programs.length - failures.length} of ${programs.length} programs built in ${context.renderer}` });
    } finally {
      context.release();
    }
  });

  const expected = cssRgb(backgroundColour);
  for (const colour of deps.backgrounds()) {
    check(colour === expected, `the page's background is ${colour}, not ${expected} as the web view's and the window's are`);
  }

  // Everything in the contract must have been exercised. selfCheckDone is called next.
  called.add('selfCheckDone');
  for (const name of Object.keys(functions) as FunctionName[]) check(called.has(name), `the self-check never calls ${name}`);
  for (const name of Object.keys(events) as EventName[]) check(deps.eventsSeen.has(name), `the page never received ${name}`);
  for (const name of Object.keys(commands) as CommandName[]) check(sent.has(name), `the self-check never sends ${name}`);

  const report: Report = { passed: failures.length === 0, checks, failures, diagnostics: deps.diagnostics() };
  await client.call('selfCheckDone', report);
  return report;
}
