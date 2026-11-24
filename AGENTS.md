# Working on CollabGod

This file is for anyone starting cold: an AI agent in a new session, a different model, or a new human contributor. It tells you where the context lives, how to pick work, and which rules are easy to break. Keep it short and current. If you add a package, a command or a rule, update this file in the same pull request.

## Read these first

1. [`README.md`](README.md): what the product is and how a lobby works.
2. [`docs/requirements.md`](docs/requirements.md): the v1 requirements. Issues refer to their IDs.
3. [`docs/roadmap.md`](docs/roadmap.md): the six phases and the gate at the end of each.
4. [`docs/adr`](docs/adr): why things are the way they are. A record marked Proposed is not settled.
5. [`CONTRIBUTING.md`](CONTRIBUTING.md): workflow, issue types and the definition of done.

Do not rely on anything outside this repository. If you learned something a later contributor will need, put it in the repo: a finding on the issue, an ADR, or a doc.

## Where the project stands

The current phase is the earliest [milestone](../../milestones) that still has open issues. Its epic issue lists the work in order. Phases 0 and 1 are specified in detail. Later phases are deliberately coarse and get refined when they start, so do not build ahead of the current phase.

## Picking work

- Take one open issue from the current milestone. One issue, one pull request.
- Before starting, check that no open pull request or branch already covers it, then comment on the issue that you are starting.
- `type: decision` issues belong to the maintainer. Prepare the options if asked. Do not decide.
- `needs: local machine` means the work depends on the maintainers' real hardware, plugins or sample libraries. From a cloud session you can write the code and the steps to run it, but you cannot finish the issue. Say so in the pull request.
- `idea` issues are parked. Do not design or build them.
- If an issue is unclear, ask on the issue instead of guessing.

## Rules that are easy to break

- **The session model is pure.** No dependency on the interface, the audio engine or Node-only APIs. It must run in a test, in the app, in the relay and in a browser.
- **Every change to a session is an operation.** There is no other way to mutate state. Operations are deterministic and define what happens when their target is gone.
- **Undo is guarded by revisions, not values.** See ADR-0004 and `spikes/session-model`. Comparing values was tried and was wrong.
- **The audio thread never blocks.** No locks, allocation, file or network calls, or logging on it.
- **A member is not a connection.** A person is a member saved in the project, with an opaque id the app makes. A dropped connection ends their presence, never their membership. Nothing in the protocol, the session format or the model may assume two people; the limit is a relay setting. See ADR-0006.
- **Samples are identified by content hash.** A local file path never goes into a session or over the network. Plugin files never leave a machine.
- **Spikes are throwaway.** Code under `spikes/` answers a question. Product code never imports from it.
- **Licensing.** The project is GPL-3.0-or-later. Do not copy code from other DAWs. A new dependency must be GPLv3-compatible and listed in `THIRD_PARTY.md` once that file exists. Everyone except Chad agrees to [`CLA.md`](CLA.md) by adding their own line to `CLA-SIGNATURES.md`, maintainers included; an agent never adds or edits a line there. Work you do for Chad is Chad's contribution and needs no line. Work you do for anyone else needs that person's own line.
- **Decisions with lasting consequences get an ADR**, opened as Proposed.

## Commands

| Where | Command | What it does |
| --- | --- | --- |
| `spikes/session-model` | `npm ci` | Install pinned dependencies (Node 22 or newer) |
| `spikes/session-model` | `npm test` | Unit, scenario and property tests |
| `spikes/session-model` | `npm run typecheck` | Strict TypeScript check |
| `spikes/session-model` | `FC_SCALE=40 npm test` | Property tests with 40 times as many random cases |
| `spikes/relay` | `npm ci` | Install pinned dependencies (Node 22 or newer) |
| `spikes/relay` | `npm test` | Relay tests over real sockets on this machine, including TLS where `openssl` is installed |
| `spikes/relay` | `npm run typecheck` | Strict TypeScript check |
| `spikes/relay` | `npm run relay -- serve` | Run a relay; `host` and `join` run the test between two locations (see its README) |
| `spikes/relay` | `deploy/gcloud.sh up` | Put the relay on a Google Cloud VM with TLS for tests between two locations; `update`, `status`, `logs` and `down` manage it (needs `gcloud` and a project; see its README). CI runs `shellcheck` on these scripts |
| `spikes/sample-index` | `npm ci` | Install pinned dependencies (Node 22 or newer) |
| `spikes/sample-index` | `npm test` | Hashing, scanning, rescans, near matches and overlap on generated files |
| `spikes/sample-index` | `npm run typecheck` | Strict TypeScript check |
| `spikes/sample-index` | `npm run samples -- scan <folder> --index <file>` | Index sample folders; `export`, `compare`, `near` and `generate` are in its README |
| `spikes/sample-transfer` | `npm ci` | Install pinned dependencies (Node 22 or newer). Run `npm ci` in `spikes/relay` too: this spike imports it |
| `spikes/sample-transfer` | `npm test` | Unit tests, and transfers through a real relay over sockets on this machine |
| `spikes/sample-transfer` | `npm run typecheck` | Strict TypeScript check |
| `spikes/sample-transfer` | `npm run transfer -- share` | `share` and `fetch` measure a transfer between two locations through a relay (see its README) |
| `spikes/plugin-inventory` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` | Configure; downloads pinned VST3 interface headers only (CMake 3.22 or newer, a C++20 compiler) |
| `spikes/plugin-inventory` | `cmake --build build --config Release --parallel` | Build `plugin_scan` and the fake test plugins, in seconds |
| `spikes/plugin-inventory` | `ctest --test-dir build -C Release --output-on-failure` | Scan fake plugins that crash, hang or misreport, each in its own process, and check the inventory |
| `spikes/plugin-inventory` | `build/plugin_scan scan --label NAME --out FILE` | Scan this machine's VST3 folders (`build/Release/plugin_scan.exe` on Windows; see its README) |
| `spikes/plugin-inventory/compare` | `npm ci`, `npm test`, `npm run typecheck` | Install, test and check the inventory comparison and identity rule (Node 22 or newer) |
| `spikes/plugin-inventory/compare` | `npm run inventory -- compare A.json B.json --out report.md` | Compare two machines' inventories; `summary A.json` checks one |
| `spikes/engine-toolchain` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` | Configure; downloads pinned JUCE and Tracktion Engine (CMake 3.22 or newer, a C++20 compiler) |
| `spikes/engine-toolchain` | `cmake --build build --config Release --parallel` | Build `engine_spike`. A cold build takes minutes; lower `--parallel` if the compiler runs out of memory |
| `spikes/engine-toolchain` | `ctest --test-dir build -C Release --output-on-failure` | Render a tone and a file offline and play them through a simulated sound card, checking both; also checks that a MIDI device change still stops playback (a known Tracktion behaviour, #101). Needs no audio device |
| `spikes/engine-projection` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` | Configure; same pinned JUCE and Tracktion Engine as `engine-toolchain`, whose `cmake/EngineDependencies.cmake` and `src/ToneCheck.cpp` it uses |
| `spikes/engine-projection` | `cmake --build build --config Release --parallel 2` | Build `engine_projection` |
| `spikes/engine-projection` | `ctest --test-dir build -C Release --output-on-failure` | Contract examples, edits during playback changing what plays, an edit storm watched by the click detector, and a rebuild while playing. Needs no audio device |
| `spikes/engine-projection/ts` | `npm ci` | Install pinned dependencies (Node 22 or newer). Imports `spikes/session-model/src`, which needs none |
| `spikes/engine-projection/ts` | `npm test` | Projection, diff and contract tests; with `ENGINE_PROJECTION_BIN` set to the built `engine_projection`, also end-to-end tests through Tracktion |
| `spikes/engine-projection/ts` | `npm run typecheck` | Strict TypeScript check |
| `spikes/engine-projection/ts` | `npm run dropout-check -- --bin <engine_projection>` | Edits the model 10 times a second while playing on the real device, and reports dropouts (see its README) |
| `spikes/webview-ui/ui` | `npm ci` | Install the page's pinned build and test tools (Node 22 or newer) |
| `spikes/webview-ui/ui` | `npm test` | Bridge contract, camera, scene, statistics and benchmark tests |
| `spikes/webview-ui/ui` | `npm run typecheck` | Strict TypeScript check |
| `spikes/webview-ui/ui` | `npm run build` | Bundle the page into `ui/dist`, which the app serves; run it before the app |
| `spikes/webview-ui` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` | Configure; same pinned JUCE and Tracktion Engine as `engine-toolchain` (`engine-toolchain/cmake/EngineDependencies.cmake`), plus the WebView2 SDK on Windows |
| `spikes/webview-ui` | `cmake --build build --config Release --parallel 2` | Build `webview_spike`. Two jobs on an 8 GB machine |
| `spikes/webview-ui` | `ctest --test-dir build -C Release --output-on-failure` | Self-test: page serving, bridge contract, engine settling, seek, looping, and resuming after a MIDI device change. Needs no window or audio device |
| `spikes/plugin-params` | `cmake -S . -B build-rules -DPLUGIN_PARAMS_CORE_ONLY=ON` then `cmake --build build-rules` and `ctest --test-dir build-rules` | The parameter and state sync rules alone, plain C++ with no JUCE: unit tests and random convergence of 2 to 4 members. About 15 seconds |
| `spikes/plugin-params` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` then `cmake --build build --config Release --parallel 2` | Configure and build the app and two versions of a test VST3; same pinned JUCE and Tracktion Engine as `engine-toolchain` |
| `spikes/plugin-params` | `ctest --test-dir build -C Release --output-on-failure` | The rules, then three members on this machine mirroring the test VST3 over TCP: sound, echoes, both people on one knob, automation, a member on version 2. Then its saved state (#13): hidden changes, hints, large states, silent presets, noise, settling, missing files, a version that refuses another's state; and the state probe on the test VST3. Needs no audio device; on Linux, a display (`xvfb-run`) |
| `spikes/plugin-params` | `build-rules/state_sync_tests --seeds 2000` | The state sync rules' random convergence with twenty times as many cases (the rules build also runs `state_sync_tests` in `ctest`) |
| `spikes/plugin-params` | `build/.../CollabGod Plugin Params Spike --probe-state <folder or .vst3>` | Measure real plugins' saved state, each in its own process; writes a Markdown report (see `README-STATE.md`) |

A new package or spike with tests gets its own job in `.github/workflows/ci.yml`, added to the `needs` list of the `CI passed` job and to its checks, and its commands get rows in this table. Code that CI does not run is not protected by the required check. Spike jobs run only when the `changes` job says something they use changed, since product code never imports a spike: the quick spike jobs (the TypeScript spikes, the plugin inventory, the plugin parameter sync rules and the shell scripts) when anything under `spikes/` or `ci.yml` changed; the engine toolchain job when `spikes/engine-toolchain/` or `ci.yml` changed; the web view and plugin parameter jobs when their own folder, `spikes/engine-toolchain/` (whose `cmake/EngineDependencies.cmake` they use) or `ci.yml` changed; and the engine projection job when `spikes/engine-projection/`, `spikes/engine-toolchain/`, `spikes/session-model/src/` (whose model its end-to-end tests drive) or `ci.yml` changed. Every job also runs on Mondays at 06:17 UTC, to catch a spike that stopped building as runners and actions changed, and when the CI workflow is started by hand from the Actions tab. A job that depends on files outside its own folder needs those paths in that check.

There is no application to build or run yet. That starts in Phase 1.

## Git and pull requests

- Never push to `main`. Branch as `feat/123-name`, `fix/123-name`, `spike/123-name`, `docs/123-name` or `chore/123-name`.
- Stage files by name. Do not use `git add -A` or `git add .`; that is how a generated test report was once committed by mistake.
- Never commit generated output, dependencies, secrets or local paths.
- Do not rewrite history on a branch someone else may have pulled.
- Commit and pull request titles follow Conventional Commits: `feat(sync): order edits through the relay`.
- Write `Closes #123` only if every acceptance criterion on the issue is met. Otherwise write `Refs #123` and list what is left.
- Use the pull request template. Under "How it was tested", say exactly what you ran and what you could not check. Do not claim a check you did not run.
- Do not merge. A maintainer reads and merges every pull request.
- Do not change repository settings, milestones or label meanings, and do not close an issue whose acceptance criteria are not all met.

## What an agent cannot verify

You cannot hear audio, see the interface on a real display, or run third-party plugins. For any change that affects sound, visuals or plugin behaviour, list the manual checks a person should do in the pull request, and do not describe the change as verified.

## Definition of done

See [`CONTRIBUTING.md`](CONTRIBUTING.md#definition-of-done). In short: acceptance criteria met and shown, tests with the change, CI green, no new warnings, docs and ADRs updated.
