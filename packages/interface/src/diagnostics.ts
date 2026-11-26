// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the page measures about the bridge, for the diagnostics panel and the app's log:
// the round trip of a ping (the bridge on its own, ADR-0002 measured 0.1 ms on Windows
// and under 1 ms on the Mac at the median), how long a command takes until a drawn frame
// includes it, and how many drawn frames had no new engine data.

import type { Diagnostics as DiagnosticsSchema, HostInfo as HostInfoSchema, Infer } from '@collabgod/bridge';
import { RollingTimings, type TimingSummary } from './stats';

export type Diagnostics = Infer<typeof DiagnosticsSchema>;
export type HostInfo = Infer<typeof HostInfoSchema>;

/** Probe commands in flight, and how long each took until a drawn frame's lastProbe included it. */
export class ProbeTracker {
  private nextId = 1;
  private readonly sentAt = new Map<number, number>();
  readonly timings = new RollingTimings(60);

  /** A new id to send, remembered with the time it was sent. */
  next(nowMs: number): number {
    const id = this.nextId++;
    this.sentAt.set(id, nowMs);
    return id;
  }

  /** A frame is being drawn: every probe up to its lastProbe has been carried out. */
  seen(lastProbe: number, nowMs: number): void {
    for (const [id, at] of this.sentAt) {
      if (id <= lastProbe) {
        this.timings.push(nowMs - at);
        this.sentAt.delete(id);
      }
    }
  }

  get pending(): number {
    return this.sentAt.size;
  }
}

const ms = (t: TimingSummary): string =>
  t.samples === 0 ? 'no samples yet' : `median ${t.medianMs.toFixed(2)} ms, p95 ${t.p95Ms.toFixed(2)} ms, max ${t.maxMs.toFixed(2)} ms (${t.samples})`;

/** The panel's text. */
export function formatDiagnostics(host: HostInfo | undefined, d: Diagnostics): string {
  const stalePercent = d.drawnFrames === 0 ? 0 : (100 * d.staleFrames) / d.drawnFrames;
  return [
    host === undefined
      ? 'Not connected to the app'
      : `CollabGod ${host.appVersion} · ${host.webView} on ${host.platform} · contract v${host.contractVersion}` +
        (host.development ? ' · development: reloads when the page is rebuilt' : ''),
    `Frames: ${d.frameDelivery}, ${d.framesPerSecond} fps drawn, ${d.staleFrames} of ${d.drawnFrames} without new engine data (${stalePercent.toFixed(1)}%)`,
    `Bridge round trip: ${ms(d.roundTrip)}`,
    `Command to drawn frame: ${ms(d.probe)}`,
    `Engine commands: ${d.commands} in ${d.commandBatches} batches`,
    'P switches push and pull · Ctrl+Shift+D (Cmd+Shift+D on a Mac) hides this',
  ].join('\n');
}
