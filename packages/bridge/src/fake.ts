// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for the app, speaking JUCE's protocol and the contract's envelope, for
// tests and for running the page in a plain browser. It answers each function with the
// handler given, or with the contract's example when there is none.

import type { JuceBackend } from './client';
import {
  type ArgsOf,
  type Command,
  type CommandName,
  commandBatchFunction,
  commands as commandDefs,
  type EventName,
  type FunctionName,
  functions,
  type ParamsOf,
  type PayloadOf,
  type ResultOf,
} from './contract';
import { decode } from './schema';

export type FakeHandlers = {
  [N in FunctionName]?: (params: ParamsOf<N>) => ResultOf<N>;
};

export type FakeCommandHandlers = {
  [C in CommandName]?: (args: ArgsOf<C>) => void;
};

export interface FakeHostOptions {
  handlers?: FakeHandlers;
  commands?: FakeCommandHandlers;
  /** Answer calls on a later turn of the event loop, as the real bridge does. Default true. */
  async?: boolean;
}

export class FakeHost implements JuceBackend {
  private readonly listeners = new Map<string, ((payload: unknown) => void)[]>();
  private readonly handlers: FakeHandlers;
  private readonly commandHandlers: FakeCommandHandlers;
  private readonly async: boolean;

  /** Every call the page made, in order, with the arguments as sent. */
  readonly calls: { name: string; params: unknown[] }[] = [];
  /** Every batch of commands the page sent. */
  readonly batches: Command[][] = [];

  constructor(options: FakeHostOptions = {}) {
    this.handlers = options.handlers ?? {};
    this.commandHandlers = options.commands ?? {};
    this.async = options.async ?? true;
  }

  addEventListener(eventId: string, fn: (payload: unknown) => void): unknown {
    const list = this.listeners.get(eventId) ?? [];
    list.push(fn);
    this.listeners.set(eventId, list);
    return list.length;
  }

  emitEvent(eventId: string, payload: unknown): void {
    if (eventId !== '__juce__invoke') return;
    const { name, params, resultId } = payload as { name: string; params: unknown[]; resultId: number };
    this.calls.push({ name, params });
    const answer = this.answer(name, params);
    const complete = (): void => this.send('__juce__complete', { promiseId: resultId, result: answer });
    if (this.async) setTimeout(complete, 0);
    else complete();
  }

  /** Sends an event to the page, as the app's generated emit functions do. */
  emit<E extends EventName>(name: E, payload: PayloadOf<E>): void {
    this.send(name, payload);
  }

  /** Sends anything at all, to test how the page copes with what the contract does not allow. */
  send(eventId: string, payload: unknown): void {
    for (const fn of this.listeners.get(eventId) ?? []) fn(structuredClone(payload));
  }

  private answer(name: string, params: unknown[]): unknown {
    try {
      if (name === commandBatchFunction) {
        const batch = (params[0] as { commands: Command[] }).commands;
        for (const command of batch) decode(commandDefs[command.type].args, command.args, `${command.type}.args`);
        this.batches.push(batch);
        for (const command of batch) (this.commandHandlers[command.type] as ((a: unknown) => void) | undefined)?.(command.args);
        return { ok: true };
      }
      if (!Object.hasOwn(functions, name)) return { ok: false, error: `there is no function named ${name}` };
      const fname = name as FunctionName;
      const def = functions[fname];
      const input = def.params === null ? undefined : decode(def.params, params[0], `${name}.params`);
      const handler = this.handlers[fname] as ((p: unknown) => unknown) | undefined;
      const result = handler !== undefined ? handler(input) : def.example.result;
      return result === undefined ? { ok: true } : { ok: true, result };
    } catch (e) {
      return { ok: false, error: e instanceof Error ? e.message : String(e) };
    }
  }
}
