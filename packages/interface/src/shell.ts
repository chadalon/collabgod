// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app shell's page: connects to the app over the bridge, keeps engine frames
// coming (pushed or pulled), draws, and shows the diagnostics panel.

import {
  BridgeClient,
  CommandQueue,
  type EventName,
  type JuceBackend,
} from '@collabgod/bridge';
import { type Diagnostics, formatDiagnostics, type HostInfo, ProbeTracker } from './diagnostics';
import { type FrameDelivery, FrameFeed } from './feed';
import type { ShaderGl } from './glsl';
import { runSelfCheck } from './selfcheck';
import { RateMeter, RollingTimings } from './stats';
import { Surface } from './surface';

export class Shell {
  private readonly client: BridgeClient;
  private readonly queue: CommandQueue;
  private readonly feed: FrameFeed;
  private readonly probes = new ProbeTracker();
  private readonly roundTrip = new RollingTimings(120);
  private readonly drawRate = new RateMeter();
  private readonly eventsSeen = new Set<EventName>();
  private pushedFrames = 0;
  private host: HostInfo | undefined;
  private nextPanelUpdate = 0;

  constructor(
    backend: JuceBackend,
    private readonly surface: Surface,
    private readonly panel: HTMLElement,
  ) {
    this.client = new BridgeClient(backend);
    this.queue = new CommandQueue((batch) => this.client.sendCommands(batch), {
      onError: (e) => this.report('error', `Commands refused: ${message(e)}`),
    });
    this.feed = new FrameFeed('pull', () => this.client.call('pullFrame'), (e) => this.report('warning', `pullFrame: ${message(e)}`));

    this.client.on(
      'frame',
      (frame) => {
        this.eventsSeen.add('frame');
        this.pushedFrames += 1;
        this.receive(frame);
      },
      (e) => this.report('error', e.message),
    );
  }

  async start(): Promise<void> {
    this.host = await this.client.call('hello');
    this.feed.setDelivery(this.host.frameDelivery);
    this.panel.hidden = !this.host.showDiagnostics;

    requestAnimationFrame((t) => this.tick(t));
    window.addEventListener('keydown', (e) => this.key(e));

    // The bridge on its own, measured once a second for the diagnostics.
    setInterval(() => void this.ping(), 1000);
    // The app keeps the latest diagnostics and writes them to its log.
    setInterval(() => void this.client.call('reportDiagnostics', this.diagnostics()).catch(() => undefined), 30_000);
    // A command's way to the app and back, measured while the panel is open.
    setInterval(() => {
      if (!this.panel.hidden) this.queue.push('probe', { id: this.probes.next(performance.now()) });
    }, 500);

    if (this.host.selfCheck) {
      const report = await runSelfCheck({
        client: this.client,
        queue: this.queue,
        feed: this.feed,
        probes: this.probes,
        host: this.host,
        eventsSeen: this.eventsSeen,
        pushedFrames: () => this.pushedFrames,
        backgrounds: () => [document.documentElement, document.body].map((el) => getComputedStyle(el).backgroundColor),
        diagnostics: () => this.diagnostics(),
        setDelivery: (d) => this.setDelivery(d),
        waitFor,
        now: () => performance.now(),
        recordRoundTrip: (ms) => this.roundTrip.push(ms),
        webgl: offscreenWebgl,
      });
      console.info('Bridge self-check', report);
    }
  }

  private receive(frame: Parameters<FrameFeed['receive']>[0]): void {
    this.feed.receive(frame, performance.now());
  }

  private tick(timestamp: number): void {
    requestAnimationFrame((t) => this.tick(t));
    this.drawRate.tick(timestamp);

    // This frame's commands go before the pull, so the frame pulled now already includes them.
    this.queue.flush();
    const frame = this.feed.beginDraw();
    // A command counts as shown when a frame that includes it is drawn, pushed or pulled alike.
    if (frame !== undefined) this.probes.seen(frame.lastProbe, performance.now());

    this.surface.draw({
      status: this.host === undefined ? 'Connecting…' : `${this.host.webView} · frames ${this.feed.frameDelivery}`,
      hostTimeMs: this.feed.hostTimeNowMs(performance.now()),
    });

    if (!this.panel.hidden && timestamp >= this.nextPanelUpdate) {
      this.nextPanelUpdate = timestamp + 250;
      this.panel.textContent = formatDiagnostics(this.host, this.diagnostics());
    }
  }

  private async ping(): Promise<void> {
    const sentAtMs = performance.now();
    try {
      await this.client.call('ping', { sentAtMs });
      this.roundTrip.push(performance.now() - sentAtMs);
    } catch (e) {
      this.report('warning', `ping: ${message(e)}`);
    }
  }

  private async setDelivery(delivery: FrameDelivery): Promise<void> {
    const answer = await this.client.call('setFrameDelivery', { frameDelivery: delivery });
    this.feed.setDelivery(answer.frameDelivery);
  }

  private diagnostics(): Diagnostics {
    return {
      frameDelivery: this.feed.frameDelivery,
      framesPerSecond: this.drawRate.perSecond(performance.now()),
      drawnFrames: this.feed.drawnFrames,
      staleFrames: this.feed.staleFrames,
      roundTrip: this.roundTrip.summary(),
      probe: this.probes.timings.summary(),
      commandBatches: this.queue.batches,
      commands: this.queue.sent,
    };
  }

  private key(e: KeyboardEvent): void {
    if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key.toLowerCase() === 'd') {
      e.preventDefault();
      this.panel.hidden = !this.panel.hidden;
      this.nextPanelUpdate = 0;
    } else if (!this.panel.hidden && !e.ctrlKey && !e.metaKey && !e.altKey && e.key.toLowerCase() === 'p') {
      void this.setDelivery(this.feed.frameDelivery === 'push' ? 'pull' : 'push').catch((err: unknown) =>
        this.report('warning', `setFrameDelivery: ${message(err)}`),
      );
    }
  }

  /** To the app's log, where a person or CI can read it. */
  report(level: 'info' | 'warning' | 'error', text: string): void {
    console.warn(text);
    void this.client.call('log', { level, message: text }).catch(() => undefined);
  }
}

function message(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

function waitFor(ready: () => boolean, timeoutMs: number): Promise<void> {
  return new Promise((resolve, reject) => {
    const start = performance.now();
    const poll = (): void => {
      if (ready()) resolve();
      else if (performance.now() - start > timeoutMs) reject(new Error(`still waiting after ${timeoutMs} ms`));
      else setTimeout(poll, 5);
    };
    poll();
  });
}

/** A WebGL2 context on a canvas of its own, for the self-check's shader step. */
function offscreenWebgl(): { gl: ShaderGl; renderer: string; release: () => void } | undefined {
  const gl = document.createElement('canvas').getContext('webgl2');
  if (gl === null) return undefined;
  // WebKit masks the GPU's name (webview-ui finding 4); WebView2 reports it.
  const info = gl.getExtension('WEBGL_debug_renderer_info');
  const renderer = String(gl.getParameter(info === null ? gl.RENDERER : info.UNMASKED_RENDERER_WEBGL));
  return { gl, renderer, release: () => gl.getExtension('WEBGL_lose_context')?.loseContext() };
}
