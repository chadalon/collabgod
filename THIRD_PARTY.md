# Third-party code

Everything CollabGod uses that it did not write, with the exact version it is pinned to. The project is GPL-3.0-or-later (ADR-0003), so every entry must be GPLv3-compatible.

To add or move a dependency, change its pin and this file in the same pull request. Nothing is bumped automatically.

## Built into the app

| Name | Pinned to | Where the pin lives | License | Notes |
| --- | --- | --- | --- | --- |
| [JUCE](https://github.com/juce-framework/JUCE) | commit [`7ff644f65d7daa1428c34b9738ebb5c449dc9172`](https://github.com/juce-framework/JUCE/commit/7ff644f65d7daa1428c34b9738ebb5c449dc9172) (9.0.5 and later `develop`, 2026-10-16) | `cmake/Dependencies.cmake` | AGPL-3.0 or the commercial JUCE 9 licence; we use AGPL-3.0 | Always the commit Tracktion Engine's `modules/juce` submodule points at (ADR-0009). JUCE's own third-party code, including the VST3 SDK, is listed in its [`JUCE.spdx.json`](https://github.com/juce-framework/JUCE/blob/7ff644f65d7daa1428c34b9738ebb5c449dc9172/JUCE.spdx.json) |
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) | commit [`a5e44996f5f08dfa6af5f5d02d900fb3cfa37422`](https://github.com/Tracktion/tracktion_engine/commit/a5e44996f5f08dfa6af5f5d02d900fb3cfa37422) (3.5.0 on `develop`, 2026-10-24) | `cmake/Dependencies.cmake` | GPL-3.0-or-later or commercial; we use GPL-3.0-or-later | Patched at configure time for Apple clang 15 (`cmake/TracktionPatches.cmake`). Its vendored code is listed below |
| [WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) (Windows only) | NuGet 1.0.3485.44, checked against its SHA-512 | `cmake/WebView2.cmake` | BSD-3-Clause style ([license](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3485.44/license)) | Only the loader is linked, statically. The WebView2 runtime itself is part of Windows 10 and 11 and is not shipped |

### Vendored inside Tracktion Engine

Tracktion Engine carries these in its source tree, so they come with the commit above. Licenses as stated in each one's files at that commit.

| Name | Folder in Tracktion Engine | License |
| --- | --- | --- |
| SoundTouch (time stretching; enabled with `TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH`) | `modules/tracktion_engine/3rd_party/soundtouch` | LGPL-2.1-or-later |
| Signalsmith Stretch | `modules/tracktion_engine/3rd_party/signalsmith-stretch` | MIT |
| Airwindows plugins | `modules/tracktion_engine/3rd_party/airwindows` | MIT |
| libsamplerate | `modules/3rd_party/libsamplerate` | BSD-2-Clause |
| CHOC | `modules/3rd_party/choc` | ISC |
| crill | `modules/3rd_party/crill` | BSL-1.0 |
| doctest | `modules/3rd_party/doctest` | MIT |
| expected | `modules/3rd_party/expected` | CC0-1.0 |
| magic_enum | `modules/3rd_party/magic_enum` | MIT |
| MPMCQueue (Erik Rigtorp) | `modules/3rd_party/rigtorp` | MIT |
| rpmalloc | `modules/3rd_party/rpmalloc` | Public domain |

## Build and test tools (not shipped)

TypeScript tools are installed by `npm ci` from the root `package-lock.json` at the exact versions below. They are declared once, in the root `package.json`, for every package in `packages/`. Their own dependencies are pinned by the lockfile.

| Name | Version | License | Used for |
| --- | --- | --- | --- |
| [TypeScript](https://www.typescriptlang.org) | 7.0.2 | Apache-2.0 | Type checking |
| [Vitest](https://vitest.dev) | 5.1.1 | MIT | Unit and property tests |
| [fast-check](https://fast-check.dev) | 4.11.0 | MIT | Property-based tests (the session model) |
| [esbuild](https://esbuild.github.io) | 0.28.2 | MIT | Bundling the interface |

Not pinned here, and not part of the product: CMake (3.22 or newer), a C++20 compiler, Node 22 or newer and git, which each developer installs (see README.md). The spikes under `spikes/` have their own lockfiles and are not covered by this file.
