// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { type Frame, FrameFeed } from '../src/feed';

const frame = (seq: number, hostTimeMs = seq * 16.7): Frame => ({ seq, hostTimeMs, lastProbe: -1 });

describe('FrameFeed', () => {
  it('counts drawn frames that had no new engine frame', () => {
    const feed = new FrameFeed('push', () => Promise.reject(new Error('not pulling')), () => undefined);
    feed.beginDraw(); // the first drawn frame starts the count
    feed.receive(frame(1), 0);
    feed.beginDraw(); // new data: fresh
    feed.beginDraw(); // nothing new: stale
    feed.receive(frame(2), 0);
    feed.receive(frame(3), 0);
    feed.beginDraw(); // fresh
    expect([feed.drawnFrames, feed.staleFrames]).toEqual([3, 1]);
    expect(feed.latest?.seq).toBe(3);
  });

  it('ignores a frame older than the latest, as when a pull and a push cross', () => {
    const feed = new FrameFeed('push', () => Promise.reject(new Error('no')), () => undefined);
    feed.receive(frame(5), 0);
    feed.receive(frame(4), 1);
    expect(feed.latest?.seq).toBe(5);
    expect(feed.received).toBe(1);
  });

  it('pulls once per drawn frame, never with one already in flight', async () => {
    let pulls = 0;
    let release: (f: Frame) => void = () => undefined;
    const feed = new FrameFeed(
      'pull',
      () => {
        pulls += 1;
        return new Promise<Frame>((resolve) => (release = resolve));
      },
      () => undefined,
    );
    feed.beginDraw();
    feed.beginDraw(); // the first pull has not come back
    expect(pulls).toBe(1);
    release(frame(1));
    await new Promise((r) => setTimeout(r, 0));
    expect(feed.latest?.seq).toBe(1);
    feed.beginDraw();
    expect(pulls).toBe(2);
  });

  it('starts counting afresh when delivery changes', () => {
    const feed = new FrameFeed('push', () => Promise.reject(new Error('no')), () => undefined);
    feed.beginDraw();
    feed.beginDraw();
    feed.setDelivery('push');
    expect([feed.drawnFrames, feed.staleFrames, feed.frameDelivery]).toEqual([0, 0, 'push']);
  });

  it("estimates the app's clock from the latest frame, at most 100 ms on", () => {
    const feed = new FrameFeed('push', () => Promise.reject(new Error('no')), () => undefined);
    expect(feed.hostTimeNowMs(0)).toBeUndefined();
    feed.receive(frame(1, 1000), 50);
    expect(feed.hostTimeNowMs(60)).toBe(1010);
    expect(feed.hostTimeNowMs(5000)).toBe(1100);
  });
});
