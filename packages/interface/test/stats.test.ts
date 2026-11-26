// SPDX-License-Identifier: GPL-3.0-or-later

import { describe, expect, it } from 'vitest';
import { ProbeTracker } from '../src/diagnostics';
import { RateMeter, RollingTimings } from '../src/stats';

describe('RollingTimings', () => {
  it('is all zero with no samples', () => {
    expect(new RollingTimings().summary()).toEqual({ samples: 0, medianMs: 0, p95Ms: 0, maxMs: 0 });
  });

  it('gives the median, 95th percentile and maximum', () => {
    const t = new RollingTimings(100);
    for (let i = 1; i <= 100; i++) t.push(i);
    expect(t.summary()).toEqual({ samples: 100, medianMs: 50, p95Ms: 95, maxMs: 100 });
  });

  it('keeps only the most recent samples', () => {
    const t = new RollingTimings(3);
    [100, 1, 2, 3].forEach((ms) => t.push(ms));
    expect(t.summary()).toEqual({ samples: 3, medianMs: 2, p95Ms: 3, maxMs: 3 });
  });

  it('ignores a timing that is not a number', () => {
    const t = new RollingTimings();
    t.push(Number.NaN);
    expect(t.summary().samples).toBe(0);
  });
});

describe('RateMeter', () => {
  it('counts the last second', () => {
    const r = new RateMeter();
    for (let t = 0; t <= 2000; t += 10) r.tick(t);
    expect(r.perSecond(2000)).toBe(101); // 1000 ms to 2000 ms, both ends
    expect(r.perSecond(5000)).toBe(0);
  });
});

describe('ProbeTracker', () => {
  it('times each probe until a frame shows it, and every one before it', () => {
    const p = new ProbeTracker();
    const a = p.next(0);
    const b = p.next(5);
    expect(b).toBe(a + 1);
    p.seen(a - 1, 10);
    expect(p.pending).toBe(2);
    p.seen(b, 20);
    expect(p.pending).toBe(0);
    expect(p.timings.summary()).toEqual({ samples: 2, medianMs: 15, p95Ms: 20, maxMs: 20 });
  });
});
