// SPDX-License-Identifier: GPL-3.0-or-later
//
// The self-check against a fake app. In the real app, `CollabGod --bridge-check` runs it
// through the real web view.

import { backgroundColour, BridgeClient, CommandQueue, contractVersion, type EventName, FakeHost, functions } from '@collabgod/bridge';
import { describe, expect, it } from 'vitest';
import { ProbeTracker } from '../src/diagnostics';
import { type Frame, type FrameDelivery, FrameFeed } from '../src/feed';
import { cssRgb, type Report, runSelfCheck } from '../src/selfcheck';

/** A fake app that pushes a frame every few milliseconds while delivery is push, and carries out probes. */
function setup(options: { background?: string; dropCommands?: boolean; skipEvents?: boolean } = {}) {
  let seq = 0;
  let lastProbe = -1;
  let delivery: FrameDelivery = 'pull';
  const reports: Report[] = [];
  const makeFrame = (): Frame => ({ seq: seq++, hostTimeMs: seq * 16.7, lastProbe });

  const host = new FakeHost({
    handlers: {
      hello: () => ({ ...functions.hello.example.result, selfCheck: true, contractVersion }),
      ping: (p) => ({ sentAtMs: p.sentAtMs, hostTimeMs: 1 }),
      pullFrame: () => makeFrame(),
      setFrameDelivery: (p) => ((delivery = p.frameDelivery), { frameDelivery: delivery }),
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
    }).finally(() => clearInterval(loop));

  return { run, reports, host };
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
});
