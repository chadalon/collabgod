# ADR-0015: The bridge contract is written once in TypeScript, and the app's C++ is generated from it

- **Status:** Accepted
- **Date:** 2026-11-25 (accepted 2026-11-26)
- **Deciders:** Chad

## Context

The interface is TypeScript in a web view and the app is C++ (ADR-0002), so every message between them is written in two languages. ADR-0002 says the bridge contract needs its own tests, and #26 asks that its messages be "defined once and type-checked on both sides".

The `webview-ui` spike wrote the messages three times: as TypeScript types and decoders, as C++ that built JSON by hand, and as example files both sides were tested against. Its C++ test could only say that a frame had the example's fields with the example's JSON types. Nothing stopped the C++ side from forgetting to register a function, reading the wrong field, or passing an integer where the page expected a string, until the page ran.

Engine-projection finding 6 adds a constraint on the shape of the bridge: every edit rebuilds Tracktion's whole playback graph on the message thread, where the bridge also runs, so commands to the engine must go at most once per display frame, in one batch.

## Decision

1. **One definition, in TypeScript**: `packages/bridge/src/contract.ts`, written with a small schema language (`schema.ts`) of booleans, finite numbers, safe integers, strings, string enums, arrays and named objects. Every field is required and unknown fields are refused. The TypeScript types come from the same values that check messages at run time.
2. **The app's C++ is generated from it** by `packages/bridge/build.mjs`, which `npm run build` runs before the C++ build. The generated `BridgeContract.h` has a struct per object, an enum class per enum, their JSON codecs, and a `Handlers` interface with one pure virtual function per function and command. The app implements `Handlers`, so a function the app forgets, or a handler with the wrong types, does not compile. The generated files are never committed.
3. **Three kinds of message.** *Functions*: the page calls and the app answers, in an envelope (`{ ok, result }` or `{ ok, error }`) so a refusal carries its reason. *Events*: the app sends, through generated emit functions. *Commands*: the page sends them to the engine, and only through `CommandQueue`, which sends everything queued in a frame as one call to the reserved function `commands`, on the next animation frame. There is no other way to send a command.
4. **Every message has an example in the contract.** The contract tests on both sides loop over the contract and check every example, and the same example with a field missing, an extra field and a field of the wrong type. `CollabGod --bridge-check` runs every message through the real web view and fails if one was never exercised. A new message is covered as soon as it is added.

## Alternatives considered

- **Write both sides by hand and test both against shared example files**, as the spike did. Two definitions that tests keep honest only as far as the examples reach, and the C++ compiler checks nothing.
- **A neutral schema file (JSON Schema, or an IDL) with generators for both languages.** It needs a generator for TypeScript types as well, or a dependency that has one, and a second build step before the page can be type-checked. TypeScript is already the language the contract is used from most, and inferring types from schema values needs no generated TypeScript at all.
- **Commit the generated header**, so a C++-only build needs no Node. The app's build already needs Node for the page, and a committed generated file goes stale unless a test compares it, which is another thing to keep in step. AGENTS.md also says not to commit generated output.
- **JUCE's `WebSliderRelay` and friends.** They tie a control to a plugin parameter, not to a session, and say nothing about the shape of a message.

## Consequences

- A change to a message is one edit in `contract.ts`, then `npm run build`. The C++ compiler then points at every handler that no longer fits.
- The C++ build needs the TypeScript build first. `npm run build` and `npm run build -- --only app` do that; configuring with CMake directly fails with a message saying so.
- The schema language is deliberately small. Optional fields, unions other than commands, maps and binary data are not in it. Sample transfer will want binary data; it does not go over this bridge (ADR-0008), but if something else needs it, the schema language grows here first, with the generator and both codecs. The first likely case is plugin state in Phase 3, which can be megabytes (#13). JUCE's bridge carries only JSON, so it would cross as base64 whatever this record says; measure first whether it should cross at all, or stay in C++ with only its hash crossing.
- Commands are fire-and-forget. A command whose effect must be seen is seen in a later frame, as the probe command is.
- The generated `dispatch` runs on the message thread. A handler must answer quickly and must never wait on the audio thread.

## How we will know

The contract tests on both sides and `--bridge-check` run in CI on Windows and macOS (#26, #20). If a bridge bug reaches a person that these should have caught, or if the generator gets in the way of a change more than it helps, this is revisited.

Accepted on 2026-11-26, after the checks on [chadalon/collabgod#156](https://github.com/chadalon/collabgod/pull/156): both maintainer machines and CI on Windows, macOS and Linux (clang 15). None of the fixes found on the way needed a change to the contract or the generator.
