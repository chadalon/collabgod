# Spike: plugin inventory and identity across two machines

Phase 0 spike for [issue #14](https://github.com/chadalon/collabgod/issues/14). A lobby may only offer a plugin both people have (PLG-2, PLG-3) and should warn when their versions differ (PLG-7). This spike scans VST3 folders on Windows and macOS without letting a bad plugin take the scanner down, records what each plugin says about itself, and compares two machines' inventories under a proposed identity rule.

It is throwaway code. The product scanner will live in the C++ app; this spike answers what to record, what counts as the same plugin, and how scanning should be isolated. The proposed decision is in [ADR-0010](../../docs/adr/0010-plugin-identity.md).

There are two parts:

- **`plugin_scan`** (C++20, no JUCE): finds VST3 bundles and loads each one in its own child process to read its class factory. It needs only Steinberg's VST3 interface headers (MIT), fetched at configure time and pinned to a commit, so it builds in seconds.
- **`compare/`** (TypeScript): reads two inventories, applies the identity rule and lists what is shared, what is not, and the surprises.

## Build and test

CMake 3.22 or newer, a C++20 compiler, Node 22 or newer.

```sh
cd spikes/plugin-inventory
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # scans the fake plugins, about 4 seconds

cd compare
npm ci
npm test              # 47 tests, under a second
npm run typecheck
FC_SCALE=40 npm test  # the property tests with 40 times as many random cases
```

The CTest builds eight fake VST3 bundles with real bundle layouts (a CFBundle on macOS, `Contents/x86_64-win/` on Windows, `Contents/x86_64-linux/` on Linux) and scans them. One is well behaved, one only offers the oldest factory interface, and the rest crash while loading, hang while loading, crash while unloading, return no factory, export no factory, or have no binary. The test checks each is recorded correctly and that the scan still finishes. CI runs this on Windows, macOS and Linux, then reads each system's fresh scan with the compare tool.

## Scan the real machines

This is what closes the issue, and it needs both maintainers' machines. On each one:

```sh
cd spikes/plugin-inventory
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

# The usual VST3 folders, or name your own folders after the options.
build/plugin_scan scan --label "chad-windows" --out chad-windows.json            # macOS and Linux
build/Release/plugin_scan.exe scan --label "chad-windows" --out chad-windows.json  # Windows (Visual Studio generator)
```

With no folders given it scans `%CommonProgramFiles%\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3` on Windows, and `/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/VST3` on macOS (`plugin_scan folders` prints them). Each plugin gets 30 seconds by default (`--timeout 60` for more). A plugin that shows a licence or activation window will sit there until the time limit and be recorded as timed out; that is a finding, not a failure of the scan. Progress goes to the terminal, one line per bundle.

Then, with both files on one machine:

```sh
cd spikes/plugin-inventory/compare
npm ci
npm run inventory -- summary ../chad-windows.json
npm run inventory -- compare ../chad-windows.json ../chad-mac.json --out report.md
```

`report.md` is ready to paste into the issue. Add `--json report.json` for the full data, or `--block-version-mismatch` to see the result under the stricter policy.

**What an inventory contains.** Plugin names, vendors, versions, class IDs, bundle paths relative to the scanned folder, the folders themselves with the home folder shown as `~`, the OS version, and for any bundle that failed the last 2 KB of what it printed, with the home folder removed. No plugin files and no absolute paths below the scanned folders. Glance at it before posting it publicly.

Do not commit the inventories: they describe someone's installed software.

## What is here

| File | What it holds |
| --- | --- |
| `scanner/Main.cpp` | `scan`, `probe` and `folders`; writes the inventory |
| `scanner/Discover.cpp` | Finds bundles under the folders without loading anything; reads `moduleinfo.json` |
| `scanner/ChildProcess.cpp` | Runs one probe with a time limit: `posix_spawn` in its own process group, or `CreateProcess` in a job object on Windows |
| `scanner/Probe.cpp` | The only code that runs a plugin: loads the module, calls its entry function, reads the factory, writes the result, then unloads |
| `scanner/Platform.cpp` | Default folders, bundle architecture folders, OS version, removing the home folder from text |
| `scanner/Json.cpp` | JSON writing, with plugin-supplied text forced to valid UTF-8 |
| `test/fake_plugin/FakeFactory.cpp` | A class factory with no plugin behind it, built in seven behaviours |
| `test/CheckInventory.cmake` | Runs the scan over the fakes and checks the inventory |
| `cmake/FakePlugins.cmake` | Builds a fake as a real VST3 bundle for the system |
| `compare/src/inventory.ts` | Validates an inventory and splits it into plugins and failures |
| `compare/src/identity.ts` | The proposed rule: identity key, version comparison, the flag-or-block setting |
| `compare/src/compare.ts` | Shared, only-on-one-side, and twelve kinds of surprise |
| `compare/src/report.ts` | The Markdown report |
| `compare/test/` | Unit and scenario tests, property tests, and the Linux scanner output as a contract fixture |

## How it works

**Out of process.** `scan` never loads a plugin. For each bundle it starts `plugin_scan probe <bundle> --out <file>` and waits up to the time limit. The probe loads the module the way a host does (`LoadLibraryEx` and `InitDll` on Windows; `CFBundle` and `bundleEntry` on macOS; `dlopen` and `ModuleEntry` on Linux), reads the factory, and writes its result through a temporary file and a rename, so the result is either complete or absent. Only then does it release the factory and call the module's exit function. So the scanner can tell four endings apart: a clean exit; a crash or hang while loading (no result); a crash or hang after reading (a result, plus an unload problem); and a probe that ran but found nothing to read (a result naming why). On Windows the probe turns off error dialogs, runs in a job object that is killed with it, and initialises COM as hosts do. On macOS and Linux it runs in its own process group, so a plugin's helper processes are killed with it.

**What is recorded for each bundle.** How the probe ended and how long it took; the factory's vendor, URL and flags; and for each class its class ID, category, name, vendor, version, SDK version, subcategories and flags, from the richest factory interface the plugin offers (`IPluginFactory3`'s Unicode info, else `IPluginFactory2`, else `IPluginFactory`, which has no vendor or version per class). Beside that: the bundle's own version (`CFBundleShortVersionString` and `CFBundleVersion` on macOS; the file and product version resource on Windows), and the bundle's `moduleinfo.json` as written.

**Class IDs.** The scanner writes each class ID in canonical form: its four 32-bit words as 32 upper-case hex digits. That is what Steinberg's `FUID::toString` prints, and it is the same on every system. The 16 raw bytes are not: Windows stores them in COM GUID byte order, with the first three fields little-endian, and macOS and Linux store them big-endian. The fake plugin declares its ID as four words, and the CTest expects the same text on all three systems, so this conversion is tested on each.

## Findings

From building and testing in a cloud container and on GitHub's Windows, macOS and Linux runners, and from reading the VST3 interfaces and JUCE 9.0.5 (the version pinned by the engine spikes). The real inventories are still to come; see the last section.

**1. Out-of-process scanning works and is cheap.** Every misbehaving fake was recorded and the scan went on. A crash on load, a hang, a crash on unload, a missing export and a missing binary each produce a different, explained result. Starting a probe and reading a trivial plugin took about 5 ms in a Linux container, 25 ms on GitHub's Windows runner and 16 to 110 ms on its macOS runner, so on a real library the cost is the plugins' own load time. The hang costs exactly the time limit. A scan of 300 plugins with one stuck on a licence dialog takes 30 seconds longer, which argues for a cache: rescan only bundles whose modified time changed, as the sample index does.

**2. Compare class IDs as four words, never as bytes.** Byte order differs between Windows and macOS (above). JUCE gets this right for its `uniqueId`, which hashes the four words, but its older `deprecatedUid` hashes the raw bytes and so differs between the two systems for the same plugin. Anything exchanged between machines must use the canonical text.

**3. Three places a plugin states its version, and they can disagree.** The factory's class version is what the plugin tells a host. The bundle's version is what the installer stamped. `moduleinfo.json` has a third. Plugins built against the oldest factory interface report no version at all. The compare tool reports each kind of disagreement on one machine and across the two, including the two cases the issue comment asks about: same release with different reported versions, and the same reported version from different releases.

**4. `moduleinfo.json` can identify a plugin without loading it, and JUCE uses it that way.** JUCE's VST3 scanner reads `moduleinfo.json` when a bundle has one and does not load the plugin. So when the file disagrees with the factory, Tracktion Engine sees the file's version of events and this scanner sees both. The compare tool reports any disagreement. The file also carries a compatibility list: class IDs a plugin declares it replaces. That is the standard way to relate two editions of a plugin, and the compare tool reports any declared replacement of a plugin the other machine has.

**5. What the engine stores is not portable.** Tracktion Engine stores a plugin by JUCE's `PluginDescription`, whose identifier string ends in a hash of the plugin's file location and a 32-bit hash of the class ID. The location part differs between machines and systems. Whatever a project saves for the engine, the identity exchanged between machines, and saved in the session, should be the class ID (ADR-0010), with each machine finding its own local install from that. This affects #12 and #13.

**6. The scanner must call the entry function.** The VST3 format requires `bundleEntry` on macOS and `ModuleEntry` on Linux before `GetPluginFactory`. The fake refuses to give out a factory until it is called, as some real plugins do, so the test checks this.

## Proposed identity rule

As in the issue comment, now in `compare/src/identity.ts` and ADR-0010:

| Two installs have | Verdict | In a lobby |
| --- | --- | --- |
| the same class ID and the same version text | identical | usable |
| the same class ID, and versions equal once trimmed with trailing `.0` parts dropped (`1.2` and `1.2.0.0`) | identical, noted | usable |
| the same class ID and different versions | version mismatch | usable but flagged (PLG-7); or blocked, if #12 and #13 show that a difference breaks sync. Chad's call |
| different class IDs | different plugins | not shared, even with the same name |

Only the Audio Module Class counts; controller classes and others in the same bundle are ignored.

## What is left on issue #14

| Done when | State |
| --- | --- |
| Scan VST3 folders on Windows and macOS and record class ID, vendor, name and version | Done. Run on a Windows 11 laptop (21 bundles) and a macOS 13 machine (16 bundles) with no failures or timeouts, in addition to the CI runs with fake plugins |
| Scan runs out of process so a crashing plugin cannot take down the app | Done and tested: crash and hang on load, crash on unload |
| Inventories from both real machines compared; mismatches and surprises listed | Done on 2026-11-14: 9 plugins shared by class ID, all 9 with different versions; 12 only on Windows, 7 only on the Mac (VST3 only: the Mac also has Serum as VST2 and Serum 1 and Kontakt as AU, which this scan does not read, #82). The report is on the issue; the inventories are not committed |
| Identity rule proposed | Proposed here and in ADR-0010. The real report agrees with it: class IDs matched across systems, while names, vendors and bundle versions did not. Flag or block stays open for #12 and #13 |
