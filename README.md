# CollabGod

A cross-platform desktop DAW where two producers edit the same session at the same time, each running their own plugins and samples locally.

> **Status: Phase 1, the walking skeleton.** Phase 0's throwaway spikes tested the risky ideas and are in `spikes/`. The app itself is being built now and does not do anything useful yet. See the [milestones](../../milestones) and [issues](../../issues).

## Why

Two producers in different cities cannot both drive the same session today. One shares a screen or a project file and the other waits. Every hand-off means uploading a project, discovering a missing plugin, and re-linking samples both people already own under different paths.

This project removes both problems:

1. **Both people edit at once.** Notes, clips, automation and plugin tweaks appear live, with visible cursors and selections.
2. **No manual file wrangling.** A lobby knows which samples and plugins each person has. It uses local copies where they exist and transfers only what is missing.
3. **Efficient by design.** Each machine renders its own audio. Only edits and missing files cross the network.
4. **A look of its own.** Sleek, fast and visually distinct.

The test for v1: two people finish a full track together without sending a file by hand.

## How a lobby works

Each machine holds the whole session, renders audio with its own plugins and finds samples in its own library. A small relay server puts both people's edits into one order, carries presence, and forwards any sample the other side lacks. The audio engine never touches the network.

The session stores references, not files:

- A clip points at a **content hash**. If your library has that audio, the clip plays from your path. If not, the file transfers once.
- A plugin is stored as its **ID, parameter values and saved state**. Each machine loads its own installed copy.

## Stack

| Part | Choice |
| --- | --- |
| Audio engine, plugin hosting | C++20, [JUCE 9](https://juce.com), [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) |
| Interface | TypeScript in JUCE's web view, timeline on a GPU canvas |
| Session model, sync, relay server | TypeScript, one package shared by app, relay and tests |

These are decisions recorded in [`docs/adr`](docs/adr). Phase 0 tested them with throwaway spikes and they were accepted at the [Phase 0 review](docs/phase-0-review.md).

## Building

You need Node 22 or newer, CMake 3.22 or newer, git, and a C++20 compiler: Visual Studio 2022 or newer on Windows, Xcode 15 or newer on macOS 12 or newer. A full developer setup guide comes with #34.

From the repository root:

```sh
npm ci          # once, and again whenever package-lock.json changes
npm run build   # builds everything: the TypeScript packages, then the app
npm test        # runs every test
```

The first `npm run build` downloads JUCE and Tracktion Engine (about 250 MB) and compiles them, which takes 5 to 15 minutes. After that only what changed is compiled.

**On a machine with 8 GB of memory, build with at most 2 parallel jobs.** Each compile job can need over 3 GB, and with 4 an 8 GB Mac stalled and restarted. The build picks a number that fits your memory by itself (2 on 8 GB) and prints it. To set it yourself: `npm run build -- --jobs 2`, or set `COLLABGOD_JOBS=2`. In PowerShell, quote the separator: `npm run build '--' --jobs 2`.

| Command | What it does |
| --- | --- |
| `npm run build:ts`, `npm run test:ts` | Only the TypeScript packages. No C++ compiler needed |
| `npm run build:app`, `npm run test:app` | Only the C++ app |
| `npm run build -- --config Debug` | A Debug build (`Release` is the default). Test it with `npm test -- --config Debug` |

The app is built in `build/`. On Windows it is `build/app/collabgod_artefacts/Release/CollabGod.exe`; on macOS, `build/app/collabgod_artefacts/Release/CollabGod.app`.

### Layout

| Folder | What it holds |
| --- | --- |
| `app/` | The desktop app, in C++ on JUCE and Tracktion Engine. Built by the root `CMakeLists.txt` |
| `cmake/` | The C++ build's shared parts: pinned dependencies, patches, checks |
| `packages/session` | The session model: operations, undo, convergence. Pure TypeScript, used by the app, the relay and the tests |
| `packages/interface` | The interface, TypeScript in the app's web view |
| `packages/relay` | The relay server |
| `scripts/` | The root build and test commands |
| `spikes/` | Phase 0's throwaway proofs. Each builds on its own; the product never imports them |
| `docs/` | Requirements, roadmap and decision records |

Third-party code and its pinned versions are listed in [`THIRD_PARTY.md`](THIRD_PARTY.md).

## Documentation

- [Requirements](docs/requirements.md): the v1 requirement set, with IDs that issues refer to
- [Roadmap](docs/roadmap.md): six phases, each ending in something usable
- [Architecture decisions](docs/adr): one short record per decision
- [Phase 0 review](docs/phase-0-review.md): what the spikes found, and what later phases inherit
- [Ideas](docs/ideas.md): things deliberately not being designed yet
- [Contributing](CONTRIBUTING.md): workflow, definition of done, engineering rules
- [Working on CollabGod](AGENTS.md): the starting point for a new contributor or AI agent

## License

Copyright (C) 2026 chadalon and contributors.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed without any warranty. See [`LICENSE`](LICENSE) for the full text and [ADR-0003](docs/adr/0003-license.md) for the reasoning.

Contributors agree to a [contributor license agreement](CLA.md) with their first pull request. You keep your copyright, and the agreement promises the project stays available as free software.
