// SPDX-License-Identifier: GPL-3.0-or-later
//
// The self-check against a fake app. In the real app, `CollabGod --bridge-check` runs it
// through the real web view.

import { backgroundColour, BridgeClient, CommandQueue, contractVersion, type EventName, FakeHost, functions } from '@collabgod/bridge';
import { describe, expect, it } from 'vitest';
import { ProbeTracker } from '../src/diagnostics';
import { type Frame, type FrameDelivery, FrameFeed } from '../src/feed';
import { controlPrograms } from '../src/glsl';
import { cssRgb, type Report, runSelfCheck } from '../src/selfcheck';
import type { ShaderProgramSource } from '../src/shaders';
import { FakeGl, type FakeGlOptions } from './fakegl';

/** A fake app that pushes a frame every few milliseconds while delivery is push, and carries out probes. */
function setup(
  options: {
    background?: string;
    dropCommands?: boolean;
    skipEvents?: boolean;
    /** The page's WebGL2; none at all when null. */
    gl?: FakeGlOptions | null;
    shaderPrograms?: readonly ShaderProgramSource[];
  } = {},
) {
  let seq = 0;
  let lastProbe = -1;
  let delivery: FrameDelivery = 'pull';
  const reports: Report[] = [];
  const logs: string[] = [];
  const fakeGl = options.gl === null ? undefined : new FakeGl(options.gl ?? { webkit: true });
  let released = 0;
  const makeFrame = (): Frame => ({ seq: seq++, hostTimeMs: seq * 16.7, lastProbe });

  const host = new FakeHost({
    handlers: {
      hello: () => ({ ...functions.hello.example.result, selfCheck: true, contractVersion }),
      ping: (p) => ({ sentAtMs: p.sentAtMs, hostTimeMs: 1 }),
      pullFrame: () => makeFrame(),
      setFrameDelivery: (p) => ((delivery = p.frameDelivery), { frameDelivery: delivery }),
      log: (p) => void logs.push(p.message),
      selfCheckDone: (r) => void reports.push(r),
    },
    commands: {
      probe: (a) => {
        if (!options.dropCommands) lastProbe = a.id;
      },
    },
  });

  const client = new BridgeClient(host);
  const queue = new CommandQueue((b) => client.sendCommands(b), {
    scheduler: {
      requestFrame: (fn) => setTimeout(fn, 1),
      cancelFrame: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
      setTimeout: (fn, ms) => setTimeout(fn, ms),
      clearTimeout: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
    },
  });
  const feed = new FrameFeed('pull', () => client.call('pullFrame'), () => undefined);
  const eventsSeen = new Set<EventName>();
  let pushed = 0;
  client.on(
    'frame',
    (f) => {
      eventsSeen.add('frame');
      pushed += 1;
      feed.receive(f, 0);
    },
    () => undefined,
  );

  // The app's display refresh and the page's drawing loop, every 2 ms.
  const loop = setInterval(() => {
    if (delivery === 'push' && !options.skipEvents) host.emit('frame', makeFrame());
    feed.beginDraw();
  }, 2);

  const run = () =>
    runSelfCheck({
      client,
      queue,
      feed,
      probes: new ProbeTracker(),
      host: { ...functions.hello.example.result, selfCheck: true },
      eventsSeen,
      pushedFrames: () => pushed,
      backgrounds: () => [options.background ?? cssRgb(backgroundColour)],
      diagnostics: () => functions.reportDiagnostics.example.params,
      setDelivery: async (d) => {
        const answer = await client.call('setFrameDelivery', { frameDelivery: d });
        feed.setDelivery(answer.frameDelivery);
      },
      waitFor: (ready, timeoutMs) =>
        new Promise((resolve, reject) => {
          const start = Date.now();
          const poll = (): void => (ready() ? resolve() : Date.now() - start > timeoutMs ? reject(new Error('timed out')) : void setTimeout(poll, 2));
          poll();
        }),
      now: () => performance.now(),
      recordRoundTrip: () => undefined,
      stepTimeoutMs: 300,
      webgl: () => (fakeGl === undefined ? undefined : { gl: fakeGl.gl, renderer: 'Fake GPU', release: () => void (released += 1) }),
      ...(options.shaderPrograms === undefined ? {} : { shaderPrograms: options.shaderPrograms }),
    }).finally(() => clearInterval(loop));

  return { run, reports, host, logs, fakeGl, released: () => released };
}

describe('runSelfCheck', () => {
  it('passes against an app that keeps the contract, and calls every function', async () => {
    const { run, reports, host } = setup();
    const report = await run();
    expect(report.failures).toEqual([]);
    expect(report.passed).toBe(true);
    expect(reports).toEqual([report]);
    const called = new Set(host.calls.map((c) => c.name));
    for (const name of Object.keys(functions)) expect(called).toContain(name);
    expect(called).toContain('commands');
  });

  it('fails when commands never reach the engine', async () => {
    const { run } = setup({ dropCommands: true });
    const report = await run();
    expect(report.passed).toBe(false);
    expect(report.failures.join('\n')).toContain('commands: timed out');
  });

  it('fails when no frame is ever pushed, and says the event never came', async () => {
    const { run } = setup({ skipEvents: true });
    const report = await run();
    expect(report.failures).toContain('the page never received frame');
  });

  it('fails when the page background differs from the contract', async () => {
    const { run } = setup({ background: 'rgb(255, 255, 255)' });
    const report = await run();
    expect(report.failures.join('\n')).toContain('background is rgb(255, 255, 255)');
  });

  it('builds every shader in the web view, gives the context back, and logs where', async () => {
    const second = { ...controlPrograms.valid, name: 'second' };
    const { run, logs, fakeGl, released } = setup({ shaderPrograms: [controlPrograms.valid, second] });
    const report = await run();
    expect(report.failures).toEqual([]);
    expect(fakeGl?.linked).toHaveLength(3); // the valid control, then both programs
    expect(fakeGl?.livePrograms.size).toBe(0);
    expect(released()).toBe(1);
    expect(logs).toContain('Shaders: 2 of 2 programs built in Fake GPU');
  });

  it('fails, naming the program, when a shader does not build in the web view', async () => {
    const broken = { ...controlPrograms.broken, name: 'timeline clips' };
    const { run, released } = setup({ shaderPrograms: [controlPrograms.valid, broken] });
    const report = await run();
    expect(report.passed).toBe(false);
    expect(report.failures).toEqual(["shaders: timeline clips: the fragment shader did not compile: ERROR: 0:4: '}' : syntax error"]);
    expect(released()).toBe(1);
  });

  it('fails when the web view has no WebGL2', async () => {
    const { run } = setup({ gl: null });
    const report = await run();
    expect(report.failures).toEqual(['shaders: no WebGL2 context, and the interface draws with WebGL2 (webview-ui finding 3)']);
  });

  it('fails when even a broken program builds, since the check would then prove nothing', async () => {
    const { run } = setup({ gl: { compiles: () => true } });
    const report = await run();
    expect(report.failures).toEqual(['shaders: a program with a syntax error built, so this check would miss a broken shader']);
  });

  it('fails when a program whose shaders do not match links, since link failures would then go unseen', async () => {
    const { run } = setup({ gl: { linksEverything: true } });
    const report = await run();
    expect(report.failures).toEqual(['shaders: a program whose shaders do not match linked, so this check would miss a link failure']);
  });

  it('fails when a valid program does not build, and gives the context back', async () => {
    const { run, released } = setup({ gl: { lost: true } });
    const report = await run();
    expect(report.failures.join('\n')).toContain('shaders: a valid program did not build');
    expect(released()).toBe(1);
  });
});
