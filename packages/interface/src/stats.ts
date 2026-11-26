// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small rolling statistics for the diagnostics panel.

import type { Infer } from '@collabgod/bridge';
import type { Timings } from '@collabgod/bridge';

export type TimingSummary = Infer<typeof Timings>;

/** The last `size` timings, summarised as median, 95th percentile and maximum. */
export class RollingTimings {
  private readonly values: number[] = [];
  private next = 0;

  constructor(private readonly size = 120) {}

  push(ms: number): void {
    if (!Number.isFinite(ms)) return;
    if (this.values.length < this.size) this.values.push(ms);
    else this.values[this.next] = ms;
    this.next = (this.next + 1) % this.size;
  }

  summary(): TimingSummary {
    const n = this.values.length;
    if (n === 0) return { samples: 0, medianMs: 0, p95Ms: 0, maxMs: 0 };
    const sorted = [...this.values].sort((a, b) => a - b);
    const at = (q: number): number => sorted[Math.min(n - 1, Math.max(0, Math.ceil(q * n) - 1))] ?? 0;
    return { samples: n, medianMs: round(at(0.5)), p95Ms: round(at(0.95)), maxMs: round(sorted[n - 1] ?? 0) };
  }
}

/** Events per second over the last second, from their timestamps. */
export class RateMeter {
  private readonly times: number[] = [];

  tick(nowMs: number): void {
    this.times.push(nowMs);
    while (this.times.length > 0 && (this.times[0] ?? 0) < nowMs - 1000) this.times.shift();
  }

  perSecond(nowMs: number): number {
    const recent = this.times.filter((t) => t >= nowMs - 1000);
    return recent.length;
  }
}

const round = (ms: number): number => Math.round(ms * 1000) / 1000;
