// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine frames for the drawing loop, pushed by the app or pulled by the page (ADR-0016).
//
// Pushed: the app sends a frame on its display refresh; the page draws with the latest.
// Pulled: as the page starts drawing a frame it asks the app for one, which arrives
// within a millisecond and is drawn with the next frame. Either way the page draws from
// the frame's timestamp, never by counting frames, because the app's refresh and the
// page's do not line up (webview-ui finding 6).
//
// It counts drawn frames that had no engine frame newer than the previous drawn frame,
// which is how push and pull are compared on real machines.

import type { Frame as FrameSchema, FrameDelivery as FrameDeliverySchema, Infer } from '@collabgod/bridge';

export type Frame = Infer<typeof FrameSchema>;
export type FrameDelivery = Infer<typeof FrameDeliverySchema>;

export class FrameFeed {
  /** The newest frame, and when it arrived on the page's clock. */
  latest: Frame | undefined;
  receivedAtMs = 0;

  /** Since delivery last changed: frames drawn, and of those, drawn with nothing new. */
  drawnFrames = 0;
  staleFrames = 0;
  /** Frames received since the page started, however they came. */
  received = 0;

  private delivery: FrameDelivery;
  private newSinceLastDraw = false;
  private drawing = false;
  private pulling = false;

  constructor(
    delivery: FrameDelivery,
    private readonly pull: () => Promise<Frame>,
    private readonly onError: (error: unknown) => void,
  ) {
    this.delivery = delivery;
  }

  get frameDelivery(): FrameDelivery {
    return this.delivery;
  }

  /** Changes delivery and starts counting afresh. Tell the app first (setFrameDelivery). */
  setDelivery(delivery: FrameDelivery): void {
    this.delivery = delivery;
    this.drawnFrames = 0;
    this.staleFrames = 0;
    this.drawing = false;
  }

  /** A frame arrived, pushed or pulled. One older than the latest is ignored. */
  receive(frame: Frame, nowMs: number): void {
    if (this.latest !== undefined && frame.seq <= this.latest.seq) return;
    this.latest = frame;
    this.receivedAtMs = nowMs;
    this.newSinceLastDraw = true;
    this.received += 1;
  }

  /** Call as each drawn frame starts. Returns the frame to draw with, and asks for the next if pulling. */
  beginDraw(): Frame | undefined {
    if (this.drawing) {
      this.drawnFrames += 1;
      if (!this.newSinceLastDraw) this.staleFrames += 1;
    }
    this.drawing = true;
    this.newSinceLastDraw = false;

    if (this.delivery === 'pull' && !this.pulling) {
      this.pulling = true;
      this.pull()
        .then((frame) => this.receive(frame, performance.now()))
        .catch(this.onError)
        .finally(() => (this.pulling = false));
    }
    return this.latest;
  }

  /** The app's clock now, estimated from the latest frame: its time plus how long ago it arrived. */
  hostTimeNowMs(nowMs: number): number | undefined {
    if (this.latest === undefined) return undefined;
    return this.latest.hostTimeMs + Math.min(Math.max(0, nowMs - this.receivedAtMs), 100);
  }
}
