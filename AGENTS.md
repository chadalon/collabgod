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
- **Every change to a session is an operation.** There is no other way to mutate state. Operations are deterministic, define what happens when their target is gone, and return an exact inverse. A new kind of operation without an exact inverse fails `packages/session`'s inverse property test.
- **Undo is guarded by revisions, not values.** See ADR-0004, ADR-0014 and `packages/session`. Comparing values was tried in the spike and was wrong.
- **The audio thread never blocks.** No locks, allocation, file or network calls, or logging on it.
- **Audio goes through `EngineHost`** (`app/src/EngineHost.h`, ADR-0018). It opens outputs only, keeps the one record of the person's choice, and says when the engine has settled. Anything that starts playback goes through `EngineHost::whenSettled`. A test never opens the machine's own sound card: it uses `SimulatedAudioDeviceType`.
- **A member is not a connection.** A person is a member saved in the project, with an opaque id the app makes. A dropped connection ends their presence, never their membership. Nothing in the protocol, the session format or the model may assume two people; the limit is a relay setting. See ADR-0006.
- **Samples are identified by content hash.** A local file path never goes into a session or over the network. Plugin files never leave a machine.
- **The project file is a contract between versions.** Changing what it holds means a new format version, a migration and a new example file kept for good. See [`docs/project-format.md`](docs/project-format.md).
- **Spikes are throwaway.** Code under `spikes/` answers a question. Product code never imports from it.
- **Licensing.** The project is GPL-3.0-or-later. Do not copy code from other DAWs. A new dependency must be GPLv3-compatible and listed in `THIRD_PARTY.md` once that file exists. Everyone except Chad agrees to [`CLA.md`](CLA.md) by adding their own line to `CLA-SIGNATURES.md`, maintainers included; an agent never adds or edits a line there. Work you do for Chad is Chad's contribution and needs no line. Work you do for anyone else needs that person's own line.
- **Every shader lives in `packages/interface/src/shaders.ts`**, so CI compiles and links it in both WKWebView and WebView2 (#20). WebKit refuses shaders that Chromium accepts.
- **CI runners have no sound card and no GPU.** A test that needs a sound card uses a simulated one, as the spikes do. A frame-rate or drawing-speed check in CI is informational, never a gate. See the tests section of `app/CMakeLists.txt`.
- **Decisions with lasting consequences get an ADR**, opened as Proposed.

## Commands

| Where | Command | What it does |
| --- | --- | --- |
| root | `npm ci` | Install the TypeScript packages' pinned dependencies (Node 22 or newer) |
| root | `npm run build` | Build everything: type check and build `packages/*`, then configure and build the app with CMake. First run downloads JUCE and Tracktion Engine and takes minutes. Picks parallel jobs from memory (2 on 8 GB); `-- --jobs N` or `COLLABGOD_JOBS=N` to change |
| root | `npm test` | Every test: the build scripts' own and the check that no product code imports a spike, every package's, then the app's with `ctest` (under `xvfb-run` on Linux). Build first |
| root | `npm run build:ts`, `npm run test:ts` | Only the TypeScript packages and build scripts; no C++ compiler needed |
| root | `npm run build:app`, `npm run test:app` | Only the app |
| root | `npm run build -- --config Debug` | A Debug build in `build/` (`Release` is the default); test it with `npm test -- --config Debug` |
| root | `npm test -w @collabgod/session` | Only the session model: unit, property, convergence and timing tests. `npm run typecheck -w @collabgod/session` checks it with no Node or browser types, so it stays pure |
| root | `FC_SCALE=40 npm test -w @collabgod/session` | The session model's property tests with 40 times as many random cases |
| root | `npm test -w @collabgod/project` | Only the project file: saving, loading, migrations, the example files, sample locations, and save-and-load of random sessions. `FC_SCALE=40` works here too |
| root | `npm run build -w @collabgod/bridge` | Generate the app's C++ side of the bridge contract into `packages/bridge/dist/cpp` (`npm run build` does this first) |
| root | `npm run dev -w @collabgod/interface` | Rebuild the interface's page on every save; a Debug build of the app, or any build with `--dev`, reloads it |
| root | `build/app/collabgod_artefacts/Release/CollabGod --bridge-check` | Open the window and run every bridge message through the real web view, on simulated sound cards, then close it; exit 0 if it passed (`ctest` runs it). `--bridge-test` runs the app's contract tests without a window, `--audio-test` the audio output tests on simulated sound cards (devices chosen, remembered, unplugged and plugged in again), and `--project-file-test` the atomic save tests, with a writer killed while it saves. `--help` lists the rest |
| root | `build/app/collabgod_artefacts/Release/CollabGod --list-audio-devices` | Print every driver type and its output devices without opening one. `--simulated-audio` runs the app on simulated sound cards, remembering nothing; `--audio-threads N` sets the audio threads for one run |
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

A new spike with tests gets its own job in `.github/workflows/ci.yml`, added to the `needs` list of the `CI passed` job and to its checks, and its commands get rows in this table. Code that CI does not run is not protected by the required check. The `workspace` job runs every package's type check and tests, and the build scripts' tests, through `npm run build:ts` and `npm run test:ts` on every push; a new package in `packages/` is covered by it with no job of its own. The `app` job builds and tests the app with `npm run build` and `npm test` on Windows and macOS, and `app-clang-15` builds and tests it on Linux with clang 15 as a stand-in for the Intel Mac's Apple clang 15 (see the comment above the job in `ci.yml`). Both run only when `app/`, `cmake/`, `CMakeLists.txt`, `scripts/`, `packages/bridge/`, `packages/interface/`, the root `package.json` or lockfile, or `ci.yml` changed. They restore the downloaded JUCE, Tracktion Engine and WebView2 sources from a cache, and on macOS and in `app-clang-15` a ccache compiler cache; only runs on `main` save them, so pull requests start from `main`'s caches. The comment above the `app` job says why and what to try next if the 15-minute budget gets tight. Spike jobs run only when the `changes` job says something they use changed, since product code never imports a spike: the quick spike jobs (the TypeScript spikes, the plugin inventory, the plugin parameter sync rules and the shell scripts) when anything under `spikes/` or `ci.yml` changed; the engine toolchain job when `spikes/engine-toolchain/` or `ci.yml` changed; the web view and plugin parameter jobs when their own folder, `spikes/engine-toolchain/` (whose `cmake/EngineDependencies.cmake` they use) or `ci.yml` changed; and the engine projection job when `spikes/engine-projection/`, `spikes/engine-toolchain/`, `spikes/session-model/src/` (whose model its end-to-end tests drive) or `ci.yml` changed. Every job also runs on Mondays at 06:17 UTC, to catch a spike that stopped building as runners and actions changed, and when the CI workflow is started by hand from the Actions tab. A job that depends on files outside its own folder needs those paths in that check.

### The product's build

- The app is C++ in `app/`, built by the root `CMakeLists.txt`. Its dependencies are pinned in `cmake/Dependencies.cmake` (ADR-0009) and listed in `THIRD_PARTY.md`; change both together. The macOS minimum is set once, in the root `CMakeLists.txt`. CI's `app-clang-15` job is what fails if a Tracktion Engine or JUCE update stops building with Apple clang 15; move a pin only when it passes.
- The app is built from two TypeScript packages, so the C++ build needs the TypeScript build first; `npm run build` and `npm run build -- --only app` do that. `packages/bridge` defines every message between the page and the app once and generates the app's C++ side (ADR-0015); change a message there, never in generated code. `packages/interface` is the page, which the app copies beside itself.
- The TypeScript packages are one npm workspace, `packages/*`, with one lockfile at the root. Build and test tools (TypeScript, Vitest, esbuild) are pinned once in the root `package.json`; a package lists only what it needs at run time. Every version is exact: `.npmrc` sets `save-exact`.
- In Windows PowerShell, quote the `--` that passes options to a script: `npm run build '--' --config Debug`. Unquoted, PowerShell drops it, npm takes the options as its own, and the script runs without them (here, a Release build). Command Prompt and Git Bash need no quotes.
- If `npm install` fails with `Cannot read properties of null (reading 'edgesOut')`, that is a bug in the npm 10 that comes with Node 22. Run `npx npm@11 install` to change the lockfile; `npm ci` with npm 10 reads the result fine.
- Spikes keep their own lockfiles and are not in the workspace. The product never imports from `spikes/`; `npm test` fails if it does.

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
