// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only way a command reaches the engine. Every edit rebuilds Tracktion's whole
// playback graph on the message thread, where the bridge also runs (engine-projection
// finding 6), so commands go at most once per display frame, all in one batch.
//
// Commands queued during a frame go together on the next animation frame. If the page
// draws no frames (a hidden or minimised window can stop requestAnimationFrame), they
// go after `fallbackMs` instead, so an edit never waits for the window to come back.

import type { ArgsOf, Command, CommandName } from './contract';
import { commands as definitions } from './contract';
import { decode } from './schema';

export interface FrameScheduler {
  requestFrame(fn: () => void): unknown;
  cancelFrame(handle: unknown): void;
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
}

/** The browser's own: requestAnimationFrame and timers. */
export const browserScheduler: FrameScheduler = {
  requestFrame: (fn) => requestAnimationFrame(fn),
  cancelFrame: (handle) => cancelAnimationFrame(handle as number),
  setTimeout: (fn, ms) => setTimeout(fn, ms),
  clearTimeout: (handle) => clearTimeout(handle as ReturnType<typeof setTimeout>),
};

export interface CommandQueueOptions {
  scheduler?: FrameScheduler;
  /** When no animation frame comes, send anyway after this long. */
  fallbackMs?: number;
  /** Told about a batch the app refused or did not answer. */
  onError?: (error: unknown, batch: readonly Command[]) => void;
}

export class CommandQueue {
  private queued: Command[] = [];
  private frameHandle: unknown = undefined;
  private timerHandle: unknown = undefined;
  private readonly scheduler: FrameScheduler;
  private readonly fallbackMs: number;
  private readonly onError: (error: unknown, batch: readonly Command[]) => void;

  /** Batches sent, and the commands in them. */
  batches = 0;
  sent = 0;

  constructor(
    private readonly send: (batch: readonly Command[]) => Promise<void>,
    options: CommandQueueOptions = {},
  ) {
    this.scheduler = options.scheduler ?? browserScheduler;
    this.fallbackMs = options.fallbackMs ?? 100;
    this.onError = options.onError ?? (() => undefined);
  }

  /** Queues a command for the next batch. Its arguments are checked against the contract now. */
  push<C extends CommandName>(type: C, args: ArgsOf<C>): void {
    const checked = decode(definitions[type].args, args, `${type}.args`);
    this.queued.push({ type, args: checked } as Command);
    if (this.frameHandle === undefined) {
      this.frameHandle = this.scheduler.requestFrame(() => this.flush());
      this.timerHandle = this.scheduler.setTimeout(() => this.flush(), this.fallbackMs);
    }
  }

  /** Commands waiting for the next batch. */
  get length(): number {
    return this.queued.length;
  }

  private flush(): void {
    if (this.frameHandle !== undefined) this.scheduler.cancelFrame(this.frameHandle);
    if (this.timerHandle !== undefined) this.scheduler.clearTimeout(this.timerHandle);
    this.frameHandle = undefined;
    this.timerHandle = undefined;
    if (this.queued.length === 0) return;

    const batch = this.queued;
    this.queued = [];
    this.batches += 1;
    this.sent += batch.length;
    this.send(batch).catch((error: unknown) => this.onError(error, batch));
  }
}
