// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every message that crosses the web view bridge between the interface (TypeScript, in
// the web view) and the app (C++). This file is the only definition: the interface's
// types and checks come from it, and `npm run build` generates the C++ side from it
// (src/cpp.ts), so the C++ compiler checks the app against it too. See README.md.
//
// Three kinds of message:
//   functions  the page calls the app and gets an answer
//   events     the app sends to the page
//   commands   the page sends to the engine. They only travel in one batch per display
//              frame (CommandQueue), through the reserved function `commands`.
//
// Every message has an example. The contract tests on both sides check every example,
// so a new message is covered as soon as it is added here.
//
// Change the contract version when a message changes shape. Both sides are built from
// this file in one build, so a mismatch means a stale page, which the page reports.

import { type Infer, type ObjectSchema, t } from './schema';

export const contractVersion = 1;

/**
 * Behind the page, in the web view and the window, so nothing flashes white before the
 * page draws or while the window resizes (webview-ui finding 11). The design system
 * (#33) owns the palette; this is the one colour the app needs before the page runs.
 */
export const backgroundColour = '#0b0d12';

// ---------------------------------------------------------------------------- types

export const WebView = t.enum('WebView', ['WebView2', 'WKWebView', 'WebKitGTK'], 'Which web view the app hosts the page in');

export const FrameDelivery = t.enum(
  'FrameDelivery',
  ['push', 'pull'],
  'How engine frames reach the page: pushed by the app on its display refresh, or pulled by the page as it starts drawing a frame (ADR-0016)',
);

export const LogLevel = t.enum('LogLevel', ['info', 'warning', 'error']);

export const HostInfo = t.object(
  'HostInfo',
  {
    contractVersion: t.integer('The contract version the app was built with'),
    appVersion: t.string(),
    platform: t.string('The operating system and its version'),
    webView: WebView,
    juceVersion: t.string(),
    engineVersion: t.string('Tracktion Engine, as the commit the app was built against'),
    development: t.boolean('The page is served from a folder on disk and reloads when it changes'),
    frameDelivery: FrameDelivery,
    showDiagnostics: t.boolean('Open the diagnostics panel at start'),
    selfCheck: t.boolean('Run the bridge self-check and report it with selfCheckDone'),
  },
  'What the app tells the page when it loads',
);

export const Frame = t.object(
  'Frame',
  {
    seq: t.integer('Counts frames built, push or pull, from 0 when the app starts'),
    hostTimeMs: t.number('Milliseconds since the app started, when this frame was built'),
    lastProbe: t.integer('The id of the last probe command the app carried out, or -1'),
  },
  'A snapshot of engine state for one drawn frame. Engine fields arrive with #27 and #30',
);

export const PingParams = t.object('PingParams', { sentAtMs: t.number('The page clock when the ping was sent') });

export const PingResult = t.object('PingResult', {
  sentAtMs: t.number('Returned unchanged'),
  hostTimeMs: t.number('Milliseconds since the app started, when it answered'),
});

export const FrameDeliverySetting = t.object('FrameDeliverySetting', { frameDelivery: FrameDelivery });

export const LogParams = t.object('LogParams', { level: LogLevel, message: t.string() });

export const Timings = t.object(
  'Timings',
  {
    samples: t.integer('How many were measured'),
    medianMs: t.number(),
    p95Ms: t.number(),
    maxMs: t.number(),
  },
  'A summary of recent timings; all 0 when there are no samples',
);

export const Diagnostics = t.object(
  'Diagnostics',
  {
    frameDelivery: FrameDelivery,
    framesPerSecond: t.number('Frames the page drew per second, recently'),
    drawnFrames: t.integer('Frames drawn since delivery last changed'),
    staleFrames: t.integer('Of those, frames drawn with no engine frame newer than the previous drawn frame'),
    roundTrip: t.doc(Timings, 'A ping to the app and back: the bridge on its own'),
    probe: t.doc(Timings, 'A probe command sent to the app until a frame shows it carried out'),
    commandBatches: t.integer('Batches of commands sent'),
    commands: t.integer('Commands sent, in those batches'),
  },
  'What the page measures about the bridge. The panel shows it and the app writes it to its log',
);

export const SelfCheckReport = t.object(
  'SelfCheckReport',
  {
    passed: t.boolean(),
    checks: t.integer('How many checks ran'),
    failures: t.array(t.string(), 'What failed, one line each'),
    diagnostics: Diagnostics,
  },
  'The result of the self-check the page runs when HostInfo.selfCheck is set',
);

export const ProbeCommand = t.object(
  'ProbeCommand',
  { id: t.integer('Chosen by the page; the next frame carries it as lastProbe') },
  'Does nothing but record its id: a command to the engine and back, measured',
);

// ---------------------------------------------------------------------------- messages

/** What a function takes or returns: an object, or nothing. */
type Shape = ObjectSchema | null;
type Value<S extends Shape> = S extends ObjectSchema ? Infer<S> : undefined;

export interface FunctionDef<P extends Shape = Shape, R extends Shape = Shape> {
  readonly doc: string;
  readonly params: P;
  readonly result: R;
  readonly example: { readonly params: Value<P>; readonly result: Value<R> };
}

export interface EventDef<P extends ObjectSchema = ObjectSchema> {
  readonly doc: string;
  readonly payload: P;
  readonly example: Infer<P>;
}

export interface CommandDef<A extends ObjectSchema = ObjectSchema> {
  readonly doc: string;
  readonly args: A;
  readonly example: Infer<A>;
}

const fn = <P extends Shape, R extends Shape>(def: FunctionDef<P, R>): FunctionDef<P, R> => def;
const event = <P extends ObjectSchema>(def: EventDef<P>): EventDef<P> => def;
const command = <A extends ObjectSchema>(def: CommandDef<A>): CommandDef<A> => def;

const exampleDiagnostics: Infer<typeof Diagnostics> = {
  frameDelivery: 'pull',
  framesPerSecond: 60,
  drawnFrames: 600,
  staleFrames: 0,
  roundTrip: { samples: 20, medianMs: 0.1, p95Ms: 0.2, maxMs: 0.4 },
  probe: { samples: 20, medianMs: 16.6, p95Ms: 17.1, maxMs: 18 },
  commandBatches: 20,
  commands: 20,
};

export const functions = {
  hello: fn({
    doc: 'The page has loaded, or reloaded. Answers with what the page needs to know about the app',
    params: null,
    result: HostInfo,
    example: {
      params: undefined,
      result: {
        contractVersion,
        appVersion: '0.1.0',
        platform: 'Windows 11',
        webView: 'WebView2',
        juceVersion: 'JUCE v9.0.5',
        engineVersion: 'a5e44996f5f0',
        development: false,
        frameDelivery: 'pull',
        showDiagnostics: false,
        selfCheck: false,
      },
    },
  }),
  ping: fn({
    doc: 'Answers at once with no engine work: the bridge round trip on its own',
    params: PingParams,
    result: PingResult,
    example: { params: { sentAtMs: 1520.25 }, result: { sentAtMs: 1520.25, hostTimeMs: 70183.5 } },
  }),
  pullFrame: fn({
    doc: 'Builds a frame now. The page calls it as it starts drawing a frame when delivery is pull',
    params: null,
    result: Frame,
    example: { params: undefined, result: { seq: 4211, hostTimeMs: 70183.25, lastProbe: 17 } },
  }),
  setFrameDelivery: fn({
    doc: 'Switches between pushed and pulled frames. Answers with the delivery now in use',
    params: FrameDeliverySetting,
    result: FrameDeliverySetting,
    example: { params: { frameDelivery: 'push' }, result: { frameDelivery: 'push' } },
  }),
  log: fn({
    doc: "Writes to the app's log, so the page's errors are where a person or CI can read them",
    params: LogParams,
    result: null,
    example: { params: { level: 'error', message: 'frame.seq is a string, not a whole number' }, result: undefined },
  }),
  reportDiagnostics: fn({
    doc: 'The page sends its diagnostics every few seconds; the app keeps the latest and writes it to its log',
    params: Diagnostics,
    result: null,
    example: { params: exampleDiagnostics, result: undefined },
  }),
  selfCheckDone: fn({
    doc: 'The self-check has finished. The app writes the report to its log and, in --bridge-check, closes its window and exits with 0 only if it passed',
    params: SelfCheckReport,
    result: null,
    example: { params: { passed: true, checks: 24, failures: [], diagnostics: exampleDiagnostics }, result: undefined },
  }),
} as const;

export const events = {
  frame: event({
    doc: 'Sent on every display refresh while delivery is push',
    payload: Frame,
    example: { seq: 4212, hostTimeMs: 70199.9, lastProbe: 17 },
  }),
} as const;

export const commands = {
  probe: command({
    doc: 'Recorded at once; the next frame shows its id. Lets the page measure a command on its way to the engine',
    args: ProbeCommand,
    example: { id: 17 },
  }),
} as const;

/** The reserved function that carries a batch of commands. */
export const commandBatchFunction = 'commands';

/** The whole contract, as one value. */
export const contract = {
  version: contractVersion,
  backgroundColour,
  functions,
  events,
  commands,
} as const;

export type Contract = typeof contract;
export type FunctionName = keyof typeof functions;
export type EventName = keyof typeof events;
export type CommandName = keyof typeof commands;

export type ParamsOf<N extends FunctionName> = Value<(typeof functions)[N]['params']>;
export type ResultOf<N extends FunctionName> = Value<(typeof functions)[N]['result']>;
export type PayloadOf<E extends EventName> = Infer<(typeof events)[E]['payload']>;
export type ArgsOf<C extends CommandName> = Infer<(typeof commands)[C]['args']>;

/** One command as it travels in a batch: `{ type: "probe", args: { id: 17 } }`. */
export type Command = { [C in CommandName]: { readonly type: C; readonly args: ArgsOf<C> } }[CommandName];

/** The parameter of the `commands` function. */
export interface CommandBatch {
  readonly commands: readonly Command[];
}

/** The answer to every function call, as it crosses the bridge. The client unwraps it. */
export type Envelope<R> = { ok: true; result?: R } | { ok: false; error: string };
