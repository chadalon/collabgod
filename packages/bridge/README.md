# @collabgod/bridge

The contract between the interface (TypeScript, in the web view) and the app (C++): every message that crosses the web view bridge, defined once in [`src/contract.ts`](src/contract.ts). The page's types and run-time checks come from it, and the build generates the app's C++ side from it, so the C++ compiler checks the app against the same definition ([ADR-0015](../../docs/adr/0015-bridge-contract.md)).

## Three kinds of message

| Kind | Direction | How |
| --- | --- | --- |
| Function | page → app → page | `client.call('ping', { sentAtMs })` returns a promise of the answer. The app answers in an envelope, `{ ok, result }` or `{ ok, error }`, which the client unwraps: a refusal becomes a `HostError` with the app's reason |
| Event | app → page | The app calls the generated `bridge::emitFrame (browser, frame)`; the page listens with `client.on('frame', fn, onError)` |
| Command | page → engine | `queue.push('probe', { id })`. `CommandQueue` sends everything queued in a display frame as one call to the reserved function `commands`, on the next animation frame, or after 100 ms if the page draws no frames. There is no other way to send a command (engine-projection finding 6) |

**One client per page.** The page has exactly one `BridgeClient`, made in `packages/interface/src/shell.ts`. Every client numbers its calls from 0, and the app's answers reach every client listening, so two clients would take each other's answers, with no error when the shapes happen to match. The same goes for JUCE's own `@juce-framework/webview` package, which counts from 0 on the same event: do not add it beside this one without giving calls ids that cannot collide. #158 makes a second client throw.

Every value is checked against the contract where it arrives, on both sides: the page checks arguments before sending and answers and events as they come; the app checks arguments before its handler runs. Every field is required and unknown fields are refused. Numbers must be finite and integers whole and within JavaScript's safe range.

## The messages

The contract file is the reference: each message has a doc comment and an example. Contract version 2:

| Message | Kind | Takes | Gives |
| --- | --- | --- | --- |
| `hello` | function | nothing | `HostInfo`: the app's version, platform, web view, whether it is a development build, frame delivery, and whether to open diagnostics or run the self-check |
| `ping` | function | `PingParams` | `PingResult`: the bridge round trip on its own |
| `pullFrame` | function | nothing | `Frame`, built now |
| `setFrameDelivery` | function | `FrameDeliverySetting` | the delivery now in use, push or pull ([ADR-0016](../../docs/adr/0016-frame-delivery.md)) |
| `audioDevices` | function | nothing | `AudioDevices`: every driver type and its output devices, scanned now, and the `AudioStatus` |
| `chooseAudioOutput` | function | `AudioChoice` | `AudioStatus` after opening it; the app remembers it. If it will not open, what was open stays open and `message` says why ([ADR-0018](../../docs/adr/0018-audio-output.md)) |
| `log` | function | `LogParams` | nothing; written to the app's log |
| `reportDiagnostics` | function | `Diagnostics` | nothing; the app keeps the latest and logs it |
| `selfCheckDone` | function | `SelfCheckReport` | nothing; ends `--bridge-check` |
| `frame` | event | | `Frame`, on every display refresh while delivery is push. Carries the late audio callback count |
| `audioChanged` | event | | `AudioStatus`, when the output changes: chosen, unplugged, back, or the engine settled |
| `probe` | command | `ProbeCommand` | nothing; the next frame's `lastProbe` shows it was carried out |

## Changing the contract

1. Edit `src/contract.ts`: the schema, the message and its example. Raise `contractVersion` if an existing message changes shape.
2. Run `npm run build` at the repository root. It regenerates the C++ and builds the app; the compiler then points at every app handler that no longer fits.
3. Implement or update the handler in `app/src/BridgeHost.cpp`, and use the message in the page.
4. If it is a new message, make `packages/interface/src/selfcheck.ts` exercise it: `--bridge-check` fails, naming it, until it does.

The contract tests on both sides loop over the contract, so the new message's example is checked, with a field missing, an extra field and a field of the wrong type, without a new test.

## The generated C++

`node build.mjs` (run by `npm run build`) writes two headers to `dist/cpp/`, which the app's CMake build includes. They are never committed.

- `BridgeContract.h`: an enum class per enum, a struct per object, their JSON codecs, the `Handlers` interface the app implements (one pure virtual function per function and command), `dispatch`, `withHandlers` to register them with the web view, and an `emit` function per event. The hand-written half of the codec is [`app/src/BridgeCodec.h`](../../app/src/BridgeCodec.h).
- `BridgeContractExamples.h`: every example, for the app's contract tests (`CollabGod --bridge-test`).

## Commands

| Command | What it does |
| --- | --- |
| `npm run build` | Generate the C++ into `dist/cpp` |
| `npm test` | Schema, contract, client, command queue and generator tests |
| `npm run typecheck` | Strict TypeScript check |

Run them from the repository root with `-w @collabgod/bridge`, or use the root `npm run build` and `npm test`.

## Testing without the app

`FakeHost` answers every function with the contract's example, or with a handler you give it, and records every call and batch. The interface's tests use it.
