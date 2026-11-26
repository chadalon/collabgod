// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract between the interface and the app. The page imports from here; the
// app gets the same contract as generated C++ (src/cpp.ts, run by build.mjs).

export * from './contract';
export { BridgeClient, BridgeTimeoutError, type ClientOptions, findJuce, HostError, type JuceBackend, type JuceGlobal } from './client';
export { browserScheduler, CommandQueue, type CommandQueueOptions, type FrameScheduler } from './commands';
export { ContractError, decode, type Infer, type Schema } from './schema';
export { FakeHost, type FakeCommandHandlers, type FakeHandlers, type FakeHostOptions } from './fake';
