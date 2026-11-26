// SPDX-License-Identifier: GPL-3.0-or-later

import { afterEach, describe, expect, it, vi } from 'vitest';
import { BridgeClient, BridgeTimeoutError, findJuce, HostError, type JuceBackend } from '../src/client';
import { events, type FunctionName, functions } from '../src/contract';
import { FakeHost } from '../src/fake';
import { ContractError } from '../src/schema';

afterEach(() => vi.useRealTimers());

/** A backend that records what is sent and answers only when told to. */
function manualBackend() {
  const listeners = new Map<string, ((p: unknown) => void)[]>();
  const sent: { name: string; params: unknown[]; resultId: number }[] = [];
  const backend: JuceBackend = {
    addEventListener: (id, fn) => listeners.set(id, [...(listeners.get(id) ?? []), fn]),
    emitEvent: (_id, payload) => sent.push(payload as (typeof sent)[number]),
  };
  const answer = (resultId: number, result: unknown): void =>
    (listeners.get('__juce__complete') ?? []).forEach((fn) => fn({ promiseId: resultId, result }));
  return { backend, sent, answer };
}

describe('BridgeClient: every function', () => {
  it.each(Object.keys(functions) as FunctionName[])('%s: sends the example and gets its answer, checked', async (name) => {
    const host = new FakeHost();
    const client = new BridgeClient(host);
    const def = functions[name];
    // Any function, by name: the loop cannot know which arguments each one takes.
    const call = client.call.bind(client) as (n: FunctionName, ...p: unknown[]) => Promise<unknown>;
    const result = await call(name, ...(def.params === null ? [] : [def.example.params]));
    expect(result).toEqual(def.example.result);
    expect(host.calls).toEqual([{ name, params: def.params === null ? [] : [def.example.params] }]);
    expect(client.pendingCount).toBe(0);
  });
});

describe('BridgeClient', () => {
  it('checks arguments before sending them, and sends nothing if they are wrong', async () => {
    const host = new FakeHost();
    const client = new BridgeClient(host);
    await expect(client.call('ping', { sentAtMs: Number.NaN })).rejects.toThrow('ping.params.sentAtMs is NaN');
    await expect(client.call('ping', { sentAtMs: 1, extra: 2 } as never)).rejects.toBeInstanceOf(ContractError);
    expect(host.calls).toHaveLength(0);
  });

  it('refuses an answer that does not match the contract', async () => {
    const host = new FakeHost({ handlers: { pullFrame: () => ({ seq: '1', hostTimeMs: 0, lastProbe: -1 }) as never } });
    const client = new BridgeClient(host);
    await expect(client.call('pullFrame')).rejects.toThrow('pullFrame.result.seq is a string, not a whole number');
  });

  it('refuses a result from a function that returns nothing', async () => {
    const host = new FakeHost({ handlers: { log: () => 1 as never } });
    await expect(new BridgeClient(host).call('log', { level: 'info', message: 'x' })).rejects.toThrow('log.result should be nothing');
  });

  it("turns the app's refusal into a HostError with its reason", async () => {
    const { backend, sent, answer } = manualBackend();
    const call = new BridgeClient(backend).call('pullFrame');
    answer(sent[0]!.resultId, { ok: false, error: 'pullFrame failed: no engine' });
    await expect(call).rejects.toThrow(HostError);
    await expect(call).rejects.toThrow('pullFrame: pullFrame failed: no engine');
  });

  it('refuses an answer that is not an envelope', async () => {
    const { backend, sent, answer } = manualBackend();
    const call = new BridgeClient(backend).call('pullFrame');
    answer(sent[0]!.resultId, { seq: 1 });
    await expect(call).rejects.toThrow('pullFrame.answer is not an envelope');
  });

  it('matches answers that come back out of order', async () => {
    const { backend, sent, answer } = manualBackend();
    const client = new BridgeClient(backend);
    const first = client.call('ping', { sentAtMs: 1 });
    const second = client.call('ping', { sentAtMs: 2 });
    answer(sent[1]!.resultId, { ok: true, result: { sentAtMs: 2, hostTimeMs: 20 } });
    answer(sent[0]!.resultId, { ok: true, result: { sentAtMs: 1, hostTimeMs: 21 } });
    expect(await first).toEqual({ sentAtMs: 1, hostTimeMs: 21 });
    expect(await second).toEqual({ sentAtMs: 2, hostTimeMs: 20 });
  });

  it('ignores answers it did not ask for', () => {
    const { backend, answer } = manualBackend();
    new BridgeClient(backend);
    expect(() => answer(99, { ok: true })).not.toThrow();
  });

  it('gives up when the app never answers', async () => {
    vi.useFakeTimers();
    const { backend } = manualBackend();
    const client = new BridgeClient(backend, { timeoutMs: 100 });
    const call = client.call('pullFrame');
    vi.advanceTimersByTime(101);
    await expect(call).rejects.toBeInstanceOf(BridgeTimeoutError);
    expect(client.pendingCount).toBe(0);
  });

  it('sends commands as one batch through the reserved function', async () => {
    const host = new FakeHost();
    await new BridgeClient(host).sendCommands([
      { type: 'probe', args: { id: 1 } },
      { type: 'probe', args: { id: 2 } },
    ]);
    expect(host.calls).toEqual([{ name: 'commands', params: [{ commands: [{ type: 'probe', args: { id: 1 } }, { type: 'probe', args: { id: 2 } }] }] }]);
  });
});

describe('BridgeClient: every event', () => {
  it.each(Object.keys(events) as (keyof typeof events)[])('%s: delivers the example, checked', (name) => {
    const host = new FakeHost({ async: false });
    const client = new BridgeClient(host);
    const received: unknown[] = [];
    const errors: ContractError[] = [];
    client.on(name, (p) => received.push(p), (e) => errors.push(e));
    host.emit(name, events[name].example);
    host.send(name, { ...events[name].example, notInTheContract: true });
    expect(received).toEqual([events[name].example]);
    expect(errors.map((e) => e.path)).toEqual([`${name}.notInTheContract`]);
  });
});

describe('findJuce', () => {
  it('finds the JUCE global only when the app injected it', () => {
    expect(findJuce({})).toBeUndefined();
    expect(findJuce({ __JUCE__: { backend: {} } })).toBeUndefined();
    const juce = { backend: { addEventListener: () => 0, emitEvent: () => undefined }, initialisationData: {} };
    expect(findJuce({ __JUCE__: juce })).toBe(juce);
  });
});
