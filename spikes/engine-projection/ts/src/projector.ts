// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Keeps the engine a projection of the model. One way only: the model is never read
 * back from the engine, and nothing the engine does changes the model.
 *
 * - `update(state)` projects the new state, diffs it against what was last sent, and
 *   sends only the difference.
 * - `rebuild()` asks the engine what it actually holds and sends whatever it takes to
 *   make that match the model. If a command ever fails, `update` does this itself.
 * - `rebuildFromScratch()` empties the engine and sends the whole projection.
 * - `verify()` compares what the engine holds with what it was sent.
 */

import type { SessionState } from '../../../session-model/src/model';
import { type ApplyOutcome, type Command } from './commands';
import { diff } from './diff';
import {
  defaultTimebase,
  type EngineDoc,
  emptyDoc,
  firstDifference,
  project,
  type SampleResolver,
  type Timebase,
} from './document';

/** What the projector needs from an engine: the real process, or a fake in tests. */
export interface EngineTarget {
  apply(commands: readonly Command[]): Promise<{ ok: boolean; failedAt?: number; error?: string; applyMs?: number; rebuildMs?: number }>;
  readback(): Promise<EngineDoc>;
}

export interface UpdateResult {
  readonly commands: number;
  /** The engine refused a command, so the projector rebuilt from readback. */
  readonly rebuilt: boolean;
  readonly missing: readonly string[];
  readonly applyMs?: number;
  readonly rebuildMs?: number;
}

export class Projector {
  /** What the engine was last told to hold. */
  private sent: EngineDoc = emptyDoc;
  private target: EngineDoc = emptyDoc;
  private missingNow: readonly string[] = [];
  private rebuilds = 0;

  constructor(
    private readonly engine: EngineTarget,
    private readonly resolve: SampleResolver,
    private readonly timebase: Timebase = defaultTimebase,
  ) {}

  get rebuildCount(): number {
    return this.rebuilds;
  }

  get missing(): readonly string[] {
    return this.missingNow;
  }

  async update(state: SessionState): Promise<UpdateResult> {
    const { doc, missing } = project(state, this.resolve, this.timebase);
    this.target = doc;
    this.missingNow = missing;
    const commands = diff(this.sent, doc);
    if (commands.length === 0) return { commands: 0, rebuilt: false, missing };

    const reply = await this.engine.apply(commands);
    if (reply.ok) {
      this.sent = doc;
      return {
        commands: commands.length,
        rebuilt: false,
        missing,
        ...(reply.applyMs === undefined ? {} : { applyMs: reply.applyMs }),
        ...(reply.rebuildMs === undefined ? {} : { rebuildMs: reply.rebuildMs }),
      };
    }

    // The engine is not what we thought. Find out what it is and fix it.
    await this.rebuild();
    return { commands: commands.length, rebuilt: true, missing };
  }

  /** Makes the engine match the model, starting from what it reports it holds. */
  async rebuild(): Promise<number> {
    this.rebuilds++;
    const actual = await this.engine.readback();
    const commands = diff(actual, this.target);
    const reply = await this.engine.apply(commands);
    if (!reply.ok) throw new Error(`rebuild failed at command ${reply.failedAt}: ${reply.error}`);
    this.sent = this.target;
    return commands.length;
  }

  /** Empties the engine and sends the whole projection. */
  async rebuildFromScratch(): Promise<number> {
    this.rebuilds++;
    const commands: Command[] = [{ op: 'reset' }, ...diff(emptyDoc, this.target)];
    const reply = await this.engine.apply(commands);
    if (!reply.ok) throw new Error(`rebuild failed at command ${reply.failedAt}: ${reply.error}`);
    this.sent = this.target;
    return commands.length;
  }

  /** The first way the engine differs from the projection, or undefined if it matches. */
  async verify(): Promise<string | undefined> {
    return firstDifference(await this.engine.readback(), this.target);
  }
}

/** An engine that exists only in memory, following the reference semantics. For tests. */
export class FakeEngine implements EngineTarget {
  doc: EngineDoc = emptyDoc;
  /** Set to make the next apply silently drop one command, to simulate drift. */
  dropNext = false;

  constructor(private readonly applyCommands: (doc: EngineDoc, commands: readonly Command[]) => ApplyOutcome) {}

  async apply(commands: readonly Command[]): Promise<{ ok: boolean; failedAt?: number; error?: string }> {
    const toRun = this.dropNext && commands.length > 0 ? commands.slice(1) : commands;
    this.dropNext = false;
    const outcome = this.applyCommands(this.doc, toRun);
    this.doc = outcome.doc;
    return outcome.failedAt === undefined
      ? { ok: true }
      : { ok: false, failedAt: outcome.failedAt, error: outcome.error ?? 'failed' };
  }

  async readback(): Promise<EngineDoc> {
    return this.doc;
  }
}
