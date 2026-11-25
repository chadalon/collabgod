// SPDX-License-Identifier: GPL-3.0-or-later
//
// The page's side of the bridge: typed calls to the app's functions and typed events
// from it, each checked against the contract as it arrives.
//
// Underneath is the object JUCE injects into the page as `window.__JUCE__`. This speaks
// the same protocol as JUCE's own @juce-framework/webview package (a function call is
// an `__juce__invoke` event, answered by `__juce__complete`), but takes the backend as
// a parameter, so tests and a plain browser can supply their own.

import {
  type Command,
  type CommandBatch,
  commandBatchFunction,
  type EventName,
  events,
  type FunctionName,
  functions,
  type ParamsOf,
  type PayloadOf,
  type ResultOf,
} from './contract';
import { ContractError, decode } from './schema';

/** The part of `window.__JUCE__.backend` the bridge uses. */
export interface JuceBackend {
  addEventListener(eventId: string, fn: (payload: unknown) => void): unknown;
  emitEvent(eventId: string, payload: unknown): void;
}

export interface JuceGlobal {
  backend: JuceBackend;
  initialisationData: { __juce__functions?: string[] } & Record<string, unknown>;
}

/** The JUCE global, if this page is running inside the app. */
export function findJuce(scope: { __JUCE__?: unknown }): JuceGlobal | undefined {
  const juce = scope.__JUCE__ as Partial<JuceGlobal> | undefined;
  if (juce === undefined || juce.backend === undefined || juce.initialisationData === undefined) return undefined;
  return juce as JuceGlobal;
}

export class BridgeTimeoutError extends Error {
  constructor(name: string, ms: number) {
    super(`The app did not answer ${name} within ${ms} ms`);
    this.name = 'BridgeTimeoutError';
  }
}

/** The app could not carry out a call: it refused the arguments, or its handler failed. */
export class HostError extends Error {
  constructor(name: string, message: string) {
    super(`${name}: ${message}`);
    this.name = 'HostError';
  }
}

interface Pending {
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

type ParamsArgs<N extends FunctionName> = ParamsOf<N> extends undefined ? [] : [params: ParamsOf<N>];

export interface ClientOptions {
  /** How long to wait for an answer. The app answers in under a millisecond; a stall is a bug. */
  timeoutMs?: number;
}

export class BridgeClient {
  private nextId = 0;
  private readonly pending = new Map<number, Pending>();
  private readonly timeoutMs: number;

  constructor(
    private readonly backend: JuceBackend,
    options: ClientOptions = {},
  ) {
    this.timeoutMs = options.timeoutMs ?? 5000;
    backend.addEventListener('__juce__complete', (payload) => {
      const { promiseId, result } = (payload ?? {}) as { promiseId?: unknown; result?: unknown };
      if (typeof promiseId !== 'number') return;
      const entry = this.pending.get(promiseId);
      if (entry === undefined) return;
      clearTimeout(entry.timer);
      this.pending.delete(promiseId);
      entry.resolve(result);
    });
  }

  /** Calls one of the app's functions and checks its answer against the contract. */
  async call<N extends FunctionName>(name: N, ...params: ParamsArgs<N>): Promise<ResultOf<N>> {
    const def = functions[name];
    if (def.params !== null) decode(def.params, params[0], `${name}.params`);
    const answer = await this.invoke(name, params);
    const result = unwrap(name, answer);
    if (def.result === null) {
      if (result !== undefined) throw new ContractError(`${name}.result`, 'should be nothing');
      return undefined as ResultOf<N>;
    }
    return decode(def.result, result, `${name}.result`) as ResultOf<N>;
  }

  /** Sends commands to the engine in one batch. Only CommandQueue should call this. */
  async sendCommands(commands: readonly Command[]): Promise<void> {
    const batch: CommandBatch = { commands };
    unwrap(commandBatchFunction, await this.invoke(commandBatchFunction, [batch]));
  }

  /**
   * Listens for an event from the app. `fn` gets payloads that match the contract;
   * `onError` gets the ContractError for one that does not.
   */
  on<E extends EventName>(name: E, fn: (payload: PayloadOf<E>) => void, onError: (error: ContractError) => void): void {
    const schema = events[name].payload;
    this.backend.addEventListener(name, (payload) => {
      let value: PayloadOf<E>;
      try {
        value = decode(schema, payload, name) as PayloadOf<E>;
      } catch (e) {
        if (e instanceof ContractError) onError(e);
        else throw e;
        return;
      }
      fn(value);
    });
  }

  /** Calls still waiting for an answer. */
  get pendingCount(): number {
    return this.pending.size;
  }

  private invoke(name: string, params: readonly unknown[]): Promise<unknown> {
    const id = this.nextId++;
    return new Promise<unknown>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new BridgeTimeoutError(name, this.timeoutMs));
      }, this.timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.backend.emitEvent('__juce__invoke', { name, params, resultId: id });
    });
  }
}

function unwrap(name: string, answer: unknown): unknown {
  if (typeof answer !== 'object' || answer === null || Array.isArray(answer)) {
    throw new ContractError(`${name}.answer`, 'is not an envelope { ok, result } or { ok, error }');
  }
  const envelope = answer as Record<string, unknown>;
  if (envelope['ok'] === true) return envelope['result'];
  if (envelope['ok'] === false && typeof envelope['error'] === 'string') throw new HostError(name, envelope['error']);
  throw new ContractError(`${name}.answer`, 'is not an envelope { ok, result } or { ok, error }');
}
