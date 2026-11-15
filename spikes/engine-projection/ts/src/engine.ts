// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Talks to the engine process (`engine_projection serve`): one JSON request per line
 * on its stdin, one reply per line on its stdout. In the app the same messages would
 * go over the web view bridge (#8); stdio stands in for it so this runs in Node.
 */

import { type ChildProcessWithoutNullStreams, spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

import type { Command } from './commands';
import type { EngineDoc } from './document';

export interface ApplyReply {
  readonly ok: boolean;
  readonly applied: number;
  readonly failedAt?: number;
  readonly error?: string;
  /** Time to change Tracktion's edit. */
  readonly applyMs: number;
  /** Time for Tracktion to rebuild its playback graph afterwards, on the engine's message thread. */
  readonly rebuildMs: number;
  readonly clips: number;
}

export interface DetectorStats {
  readonly armed: boolean;
  readonly seconds: number;
  readonly probeClicks: number;
  readonly probeGaps: number;
  readonly maxProbeStep: number;
  readonly clipEdges: number;
  readonly eventEditTimes: readonly number[];
}

export interface EngineStats {
  readonly settled: boolean;
  readonly settleMs: number;
  readonly playing: boolean;
  readonly audibleSec: number;
  readonly unexpectedStops: number;
  /** The device's own dropout counter; -1 when it has none, or with the simulated sound card. */
  readonly xruns: number;
  readonly cpu: number;
  /** Simulated sound card only: the slowest block, and how many ran over real time. -1 otherwise. */
  readonly maxBlockMs: number;
  readonly lateBlocks: number;
  readonly detector: DetectorStats;
}

export interface AudioInfo {
  readonly simulated: boolean;
  readonly type: string;
  readonly name: string;
  readonly sampleRate: number;
  readonly blockSize: number;
  readonly audioCpus: number;
  readonly machineCpus: number;
}

/** What the engine sent to the device, measured on its output (src/OutputAnalysis.h). Levels in dBFS; null for silence. */
export interface OutputAnalysis {
  readonly seconds: number;
  readonly windows: number;
  readonly windowSeconds: number;
  /** The probe tone's usual level on the left (median over the windows). */
  readonly probeDbfs: number | null;
  readonly probeLowestDbfs: number | null;
  /** Seconds into the recording. */
  readonly probeLowestAt: number;
  /** Windows where the probe was more than `dipDb` below its usual level. */
  readonly dipWindows: number;
  readonly dipDb: number;
  /** The loudest window of anything on the left that is not the probe. */
  readonly leftResidualMaxDbfs: number | null;
  readonly leftResidualMaxAt: number;
  /** Worst ratio of non-probe signal on the left to the right channel, in dB, where the right was louder than -40 dBFS. */
  readonly crosstalkDb: number | null;
  readonly crosstalkAt: number;
  readonly leftPeakDbfs: number | null;
  readonly rightPeakDbfs: number | null;
  readonly samplesOverFullScale: number;
}

export interface RecordingReply {
  readonly sampleRate: number;
  readonly analysis: OutputAnalysis;
  /** Where the WAV was written, when one was asked for. */
  readonly file?: string | null;
  readonly error?: string;
}

export interface EngineOptions {
  readonly simulatedAudio?: boolean;
  /** Extra arguments for `serve`, such as `--tracktion-wave-nodes`, `--audio-cpus 1` or `--device-type <name>`. */
  readonly extraArgs?: readonly string[];
  /** Where the engine's log (stderr) goes. Default: discarded. */
  readonly onLog?: (line: string) => void;
}

type Pending = { resolve: (reply: Record<string, unknown>) => void; reject: (error: Error) => void };

export class EngineProcess {
  private readonly child: ChildProcessWithoutNullStreams;
  private readonly pending = new Map<number, Pending>();
  private nextSeq = 1;
  private exited = false;

  constructor(binary: string, options: EngineOptions = {}) {
    const args = ['serve', ...(options.simulatedAudio ? ['--simulated-audio'] : []), ...(options.extraArgs ?? [])];
    this.child = spawn(binary, args, { stdio: ['pipe', 'pipe', 'pipe'] });

    createInterface({ input: this.child.stdout }).on('line', (line) => {
      let reply: Record<string, unknown>;
      try {
        reply = JSON.parse(line) as Record<string, unknown>;
      } catch {
        options.onLog?.(`(stdout) ${line}`);
        return;
      }
      const seq = typeof reply['seq'] === 'number' ? reply['seq'] : -1;
      const waiting = this.pending.get(seq);
      if (waiting) {
        this.pending.delete(seq);
        waiting.resolve(reply);
      }
    });

    createInterface({ input: this.child.stderr }).on('line', (line) => options.onLog?.(line));

    this.child.on('exit', (code, signal) => {
      this.exited = true;
      const error = new Error(`engine exited (code ${code}, signal ${signal})`);
      for (const waiting of this.pending.values()) waiting.reject(error);
      this.pending.clear();
    });
  }

  private request(type: string, body: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    if (this.exited) return Promise.reject(new Error('engine has exited'));
    const seq = this.nextSeq++;
    return new Promise((resolve, reject) => {
      this.pending.set(seq, { resolve, reject });
      this.child.stdin.write(`${JSON.stringify({ seq, type, ...body })}\n`);
    });
  }

  async hello(): Promise<AudioInfo> {
    const reply = await this.request('hello');
    return reply['audio'] as AudioInfo;
  }

  async apply(commands: readonly Command[]): Promise<ApplyReply> {
    return (await this.request('apply', { commands })) as unknown as ApplyReply;
  }

  async readback(): Promise<EngineDoc> {
    const reply = await this.request('readback');
    return reply['doc'] as EngineDoc;
  }

  async transport(change: { play?: boolean; position?: number; loop?: readonly [number, number] | null }): Promise<void> {
    await this.request('transport', change);
  }

  /** Resets the click detector's counts and the simulated sound card's block times. */
  async arm(): Promise<void> {
    await this.request('arm');
  }

  /** Starts recording the output as it goes to the device, for up to `seconds`. */
  async record(seconds: number): Promise<void> {
    await this.request('record', { seconds });
  }

  /** Analyses what has been recorded so far, and writes it as a WAV if `file` (an absolute path) is given. */
  async recording(file?: string): Promise<RecordingReply> {
    return (await this.request('recording', file === undefined ? {} : { file })) as unknown as RecordingReply;
  }

  async stats(): Promise<EngineStats> {
    return (await this.request('stats')) as unknown as EngineStats;
  }

  async waitUntilSettled(timeoutMs = 15_000): Promise<EngineStats> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const stats = await this.stats();
      if (stats.settled) return stats;
      if (Date.now() > deadline) throw new Error('engine did not settle');
      await new Promise((r) => setTimeout(r, 50));
    }
  }

  async quit(): Promise<void> {
    if (this.exited) return;
    const exited = new Promise<void>((resolve) => this.child.once('exit', () => resolve()));
    await this.request('quit').catch(() => undefined);
    this.child.stdin.end();
    await Promise.race([exited, new Promise((r) => setTimeout(r, 5000))]);
    if (!this.exited) this.child.kill();
  }
}
