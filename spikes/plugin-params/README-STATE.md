# Spike: sync plugin state that is not a parameter

Phase 0 spike for [issue #13](https://github.com/chadalon/collabgod/issues/13) (PLG-6). Parameter sync ([issue #12](https://github.com/chadalon/collabgod/issues/12), [`README.md`](README.md)) gives two people the same knob values, but Serum 2 keeps its LFO shapes, modulation and wavetables outside its parameters, and Lounge Lizard changes its parameters on a preset load without telling the host. The only thing a host can do with that part of a plugin is save the plugin's whole state and load it somewhere else. This spike does that between members, measures what it costs, and tries how far a generic way of noticing that the state changed can go.

It builds on the parameter spike in the same folder, because state and parameters have to agree with each other: the same app, the same test VST3 (with a hidden part added), and the same TCP link. It is throwaway code. Product code never includes it.

## Where the issue stands

| Done when | Status |
| --- | --- |
| A preset loaded in one instance appears in the other by sending the plugin's saved state | **Works with the test VST3** (a hidden wavetable, a preset that moves parameters silently, large embedded data), three members on this machine. Real plugins: not run yet; steps below |
| A way to detect that non-parameter state changed, or a fallback such as periodic state hashing | **Built and measured**: hints from the plugin, the mouse let go in its window, and budgeted polling with hashing. How generic it can be: finding 2 |
| State size measured for Serum 2 and at least two other plugins | **Measured on Windows** for Serum 2 and 27 others (finding 11). The Mac still to run |
| Behaviour when a preset references a file the other machine lacks is documented | **Documented for the test VST3, and the host detects it** (finding 8). Real plugins: checklist below |
| Recommendation written for PLG-6 | **Written** ([below](#recommendation-for-plg-6) and [ADR-0011](../../docs/adr/0011-plugin-state-sync.md), Proposed). Provisional until the real plugins are measured |
| Plugin versions (the comment on the issue) | **The harness classifies every load** (finding 9); the test VST3 shows a silent refusal. Real version pairs: checklist below |

## How state sync works

The rules are in `src/core/StateSync.*`, plain C++ with no JUCE, beside the parameter rules and on the same Lamport clock.

1. **The hidden part of a plugin is one value.** The newest announced state wins as a whole, ordered by stamp. Two people changing it at once do not merge: the one announced later wins, and if a state arrives while a change here is still waiting to be announced, ours goes out at once with a newer stamp. Nothing assumes two people.
2. **Parameters keep their own order.** A state travels with its sender's parameter values and stamps from when it was read. After loading it, a parameter someone changed since then is set back to the newer value. A parameter the loaded state holds differently from its sender's snapshot was changed by the plugin without telling the host: it counts as written at the state's stamp and is sent on as a parameter change, so the sender's host learns it too.
3. **Changes are found by reading.** The host reads the state (`getStateInformation`) and hashes it when something hints that it changed: the plugin calls VST3 `setDirty`, restarts or changes program, floods the host with parameter reports that change nothing (what Serum 2 does while an LFO is edited), or the person lets go of the mouse in the plugin's window. Polling can be added, all the time or only while the window is open; it slows itself down so reads take at most 2 % of the time.
4. **A change is announced once it is still.** The state must read the same for 300 ms, with no parameter moved here for 100 ms and no control held. A burst of edits becomes one announcement.
5. **An announcement carries the hash, not the bytes.** A member whose plugin already saves that hash fetches nothing: the change was only parameters, which parameter sync carried. Otherwise it reads once more after 150 ms (a value set on a plugin reaches its saved state only when it next processes audio) and then asks for the bytes.
6. **The bytes go only to whoever asks**, zlib-compressed, in chunks, paced so parameter messages are not stuck behind them. The receiver checks the SHA-256, waits while someone here holds a control, then loads them.
7. **Loading never echoes.** For 1.5 s after a load (or two polling intervals, if longer), changes in the state are the plugin settling, not news, unless a person does something here first. A change seen straight after parameters arrived from someone else, with nothing done here, is those parameters. A plugin whose state differs on every read with nothing done is marked noisy and is not polled. If a load put an older value into a parameter and the newer one was set back, the plugin reporting the older value later is an echo, and the newer one is set again: the first such report within 30 s, once.
8. **A member who joins** is told the agreed state, under its stamp, by everyone who has it. An untouched state has stamp 0, so any edited one wins over it.

## What is here

| Path | What it holds |
| --- | --- |
| `src/core/StateSync.*` | The rules above, a SHA-256, the search for file paths in a state, and the classification of a load |
| `src/core/ParamSync.*` | The parameter rules, with what state sync needs from them: a shared clock, snapshots, and adopting a loaded state |
| `tests/StateSyncTests.cpp` | Unit tests, and a random test: 2 to 4 members moving knobs, editing the hidden part and loading presets at once through a relay with random delays, with plugins that save differently by version, lag, rewrite their state after a load, or change parameters silently. All must end the same and go quiet |
| `src/host/StateHost.*` | One member's state: reads, hints, chunks, loads, the checks after a load |
| `src/host/Wire.*` | The state messages: `state`, `stateWant`, `stateData`, `stateMissing` |
| `src/selftest/StateSelfTest.cpp` | Three members over TCP on this machine with the test VST3, through every case below. `ctest` runs it |
| `src/probe/StateProbe.cpp` | `--probe-state`: measures real plugins' state, each in its own process |
| `src/app/Main.cpp` | The window, now with a **Sync state** switch, the detection choice, **Send state now** and **Read state now**, and a state section in the counters and the report |
| `test-plugin/TestSynth.cpp` | The test VST3, now with a hidden wavetable and the test controls listed at its top |

## Build and run

As in [`README.md`](README.md#build-and-run-it). The rules alone, in seconds:

```sh
cd spikes/plugin-params
cmake -S . -B build-rules -DPLUGIN_PARAMS_CORE_ONLY=ON
cmake --build build-rules
ctest --test-dir build-rules --output-on-failure
build-rules/state_sync_tests --seeds 2000     # the random test with twenty times as many cases
```

The full build adds two tests to `ctest`: `state_between_members` (about 40 seconds; prints `PASS`, `FAIL` and `STATE` lines) and `state_probe` (the probe run on the test VST3). On Linux the self-tests open the test plugin's window, so they need a display (`xvfb-run` in a cloud session).

## Measure real plugins

This is the part a cloud session cannot do. Allow an hour per machine.

### 1. The probe, for sizes and behaviour (10 minutes)

On each machine, after building. If `build` is older than the probe (it came with #13), configure it again before building: a build alone does not pick up the new source files, and the old app ignores `--probe-state` and opens its window instead. Close the app first on Windows, or the build cannot replace it.

```sh
# Windows (PowerShell). The app is a Windows GUI program: without `| Out-Host`, PowerShell returns
# at once and shows no output while the probe runs in the background.
& "build\plugin_params_spike_artefacts\Release\CollabGod Plugin Params Spike.exe" --probe-state "C:\Program Files\Common Files\VST3" --try-bad-state | Out-Host
# macOS
"build/plugin_params_spike_artefacts/Release/CollabGod Plugin Params Spike.app/Contents/MacOS/CollabGod Plugin Params Spike" --probe-state /Library/Audio/Plug-Ins/VST3 --try-bad-state
```

It loads every plugin in the folder twice, each plugin in its own process (a crash or a two-minute hang only costs that row), and writes a Markdown table to `Documents/CollabGod Spike Reports/plugin-state/probe-<date>.md`, also copied to the clipboard. Paste it into the issue. A licence dialog may appear for some plugins; close it. Drop `--try-bad-state` if a plugin crashes on it and you want its other numbers.

What each column answers:

| Column | Why it matters |
| --- | --- |
| State, compressed | What a transfer costs. The issue asks for Serum 2 and two others |
| Read ms | What a read costs the message thread, so whether polling is affordable |
| Load ms | How long the other person's plugin is busy taking a state |
| Same bytes twice | If not, the plugin cannot be polled (a counter or a time in its state) |
| Two fresh copies match | If not, two untouched copies already differ, and every join transfers a state |
| Load own state: saves it back | If not, loading changes something, and a load can never be checked by hash |
| Load into another copy: saves it back | If yes, members can compare states by hash and skip transfers. If not, every announcement means a transfer |
| Changes after a load | A plugin that keeps loading in the background; the settle window must cover it |
| Hints while probing | Whether it ever calls `setDirty` or restarts. Low numbers here are normal: nothing was edited |
| Paths in the state | Files it names, which another machine may lack (home folder shown as `~`) |
| Truncated state | Whether a broken state is refused or taken silently |

### 2. Two copies, by hand (40 minutes)

Start two copies as in [`README.md`](README.md#1-start-two-copies), on one machine or two, load the same plugin in both, press **Play** in both (until then the plugin is not processing: Serum 2 says "DAW suspended processing"), connect, and leave **Sync state** on with **Hints, and poll while the window is open**. Open the plugin window in both. For Serum 2, then one other synth and one effect:

- [ ] **Preset.** Load a factory preset in one copy. The other shows it (same name, same sound) within about a second. Note the time, and `last transfer` in the counters.
- [ ] **LFO shape.** Drag an LFO point in Serum. Let go. The other copy's LFO follows. Note which hint found it: `hints: unchanged parameter reports`, `hints: setDirty` or `hints: mouse let go in the window`.
- [ ] **Modulation.** Drag an LFO onto a knob. The other copy shows the assignment.
- [ ] **Wavetable.** Pick another wavetable, then import one of your own (a file the other machine does not have). Note what the other copy does with the imported one: plays it (the data is in the state), shows an error, or plays something else. The log says if the state names a file this machine lacks.
- [ ] **Sampler or a plugin with samples.** If you have one (Kontakt, a drum sampler, Serum's noise oscillator with your own file), load a sample that only one machine has. Note what the other plugin shows.
- [ ] **Hints only.** Set the detection to **Find state changes from hints** and repeat the LFO edit. If it still syncs, the plugin hints. If it does not, try **Read state now**.
- [ ] **No echo.** Watch both copies for ten seconds after a change: `announced` stops rising, and nothing goes back and forth.
- [ ] **Both at once.** Each person changes the preset at the same moment. Both end on the same one.
- [ ] **Knob only.** Turn a knob. `already had it (same hash)` rises and `state data received` does not, or note that it does.
- [ ] **A machine with another version** of the same plugin (the #14 pairs: FabFilter, Melodyne, Ozone 9, VocalSynth 2). Load a preset on the newer one, then on the older one. The log says, for each load, `loaded exactly`, `loaded, saved back differently`, `loaded with different parameters (a silent difference)` or `not loaded: the plugin's state did not change`. Note whether the plugin itself showed an error (loud) or not (silent), and whether it sounds the same.

Then **Save report** in each copy and paste it into the issue. Add a row per plugin:

| Plugin | Version, OS | State size (raw, compressed) | Preset follows | LFO / modulation / wavetable follow | Hint that found it | Own file on one machine | Other version: newer to older, older to newer | Echoes or loops | Other |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CollabGod Test Synth (this repo) | 1.0.0 and 2.0.0, Linux (cloud) | 266 B, 191 B; up to 10.67 MiB with embedded data | yes, ~0.5 s | wavetable yes | setDirty, mouse, polling | plugin flags the sample missing, host warns | older loads newer: refused silently; newer loads older: yes | none | parameters it moves silently are found |
| | | | | | | | | | |

## Findings

Findings 1 to 10 are from a cloud session on Linux (2 cores) with the test VST3 and the rules' random test. Finding 11 is the probe on a maintainer's Windows machine with real plugins. Real plugins have not been through the checklist yet.

1. **A saved state carries what parameters do not.** A new wavetable in A's plugin, which no parameter describes, reached B as A's saved state and B sounded the same: every one of the first eight harmonics within 0.0001 of A's, where before the change they differed by up to 0.45. From the change to B playing it took about 0.5 s, nearly all of it on purpose: 30 ms for the hint, 300 ms for the change to be still, 150 ms for B to read again. The transfer itself, 266 bytes, took 6 ms.

2. **A generic detector exists, and its limits are cost and certainty, not reach.** The issue's comment asks how feasible generic detection is. Reading the state and hashing it finds every change, from any plugin, with no help from it: polling found the test VST3's change with every hint off in about 0.8 s at a 500 ms interval. What it costs:
   - **Reads are on the message thread** (VST3 requires it; JUCE takes the message manager lock), and they scale with the state: 0.07 ms for 266 bytes, about 20 ms for 1.3 MiB, 120 to 150 ms for 10.7 MiB on this machine. A 150 ms read is a visible hitch in the interface. Polling is budgeted to 2 % of the time, so a 10 MiB state is read about every 7 seconds; a 266 byte one every 500 ms.
   - **A hash says something changed, not what.** Every knob move changes it too. Finding 3 is how that costs nothing.
   - **Some states change with nothing done** (a counter, a time). Two reads in a row at load time, and four unprompted changes in a row later, mark such a plugin noisy, and it is then not polled. The self-test's noisy plugin was caught and sent nothing.
   - **Polling cannot tell a person's edit from the plugin settling after a load.** With no signal at all, the random test lost an edit made in the 1.5 s after a state arrived in 2.5 to 7 % of runs (2,000 runs each for 2, 3 and 4 members). Any sign of a person removes that: with the mouse seen in the window, or a parameter moved here, no run lost one.

   So the detector worth building is generic but layered: hints when the plugin gives them (`setDirty`, a restart, a burst of unchanged parameter reports), the mouse let go in the plugin's window (the host asks the system for the button state and pointer position, so it works whatever the plugin's interface is), and polling only while the plugin's window is open, budgeted by read cost. The mouse covers what Serum 2 is known to leave unreported; whether Serum calls `setDirty` at all is in the checklist. JUCE turns `restartComponent` flags into one "program changed" signal, so the host cannot tell a restart for parameter values from one for latency; the product, if it talks to VST3 directly, can.

3. **Most changes need no transfer.** A state announcement is a hash. After A swept a knob with polling on, A's state changed and was announced, and B's plugin already saved the same hash: 1 announcement matched, 0 bytes of state sent. B also took 2 of its own state changes as A's parameters, not news. Whether real plugins save byte-identical states for the same content decides how often this works: the probe's "Load into another copy" column. On Windows, 23 of 28 plugins did, Serum 2 among them (finding 11).

4. **State and parameters can be kept consistent, and it takes care.** The random test found three ways to get it wrong before it passed: a state written while a knob was still on its way put the old value back everywhere; a parameter a plugin changed silently was known only to whoever loaded the state, not to the plugin's own host; and a plugin reporting the values it had just loaded, 2.3 s later on this machine and over 5 s later on GitHub's macOS runner while the message thread was busy with a 10 MiB load, was sent as a new change and undid a knob moved meanwhile. The rules now handle each, and the random test (8 settings, 2 to 4 members, 2,000 runs each) converges every time. In the self-test, a preset that moved Level and Pitch without telling A's host was found by B on loading it (2 parameters) and sent back, and A's host then showed the right values.

5. **Large states work, and parameters do not wait for them.** With 8 MiB of random data embedded, a Level change made while the state was on its way reached B within 30 to 100 ms; the state took about 1 s more to arrive (paced at 32 KiB every 5 ms for the test), and B kept the new Level after loading it. Loading the 10.7 MiB state took 148 ms. From the change to B having it took about 4 to 5 s, most of it reads of 120 to 150 ms each; on GitHub's macOS runner, 13.5 s, with single reads of up to 0.55 s. Windows and macOS CI runners pass the same self-test.

6. **JUCE's host wraps a VST3 state in XML with base64**, so 8 MiB of plugin data became a 10.67 MiB blob, and zlib brought it back to 8.08 MiB (random data does not compress). The product should save and send the component and controller states as raw bytes, as a `.vstpreset` does, not JUCE's wrapper. Through JSON with base64 the spike sent 12.1 MiB for it; the relay's binary frames would send 8.08.

7. **Loading is all or nothing, and a person's hand wins.** A state is not loaded while anyone on that machine holds a control; it is loaded when they let go, and their final value is set again afterwards. A plugin that rewrites its state 300 ms after a load (as one finishing in the background would) was loaded once, and nothing came back: the rewrite was taken as settling.

8. **A state can name files, and that is outside our control.** The test VST3 refers to a sample by its path. When B loaded A's state and the file was not on B's machine, the plugin said so (its "sample missing" flag), and B's host, which looks for path-like text in the component and controller states, warned: `1 file(s) the state refers to are not on this machine: /tmp/collabgod-test-synth/sample-….wav`. Three things follow:
   - Plugins differ: some embed the data in their state, some keep only a path (samplers usually do), some keep both. The plugin decides, not the host; the checklist finds out for Serum 2's imported wavetables.
   - **A state with a path breaks the rule that a local path never goes over the network** (AGENTS.md). The host cannot rewrite an opaque state, so the product either accepts that plugin state is the plugin's own data and says so, or refuses to sync it. That is a decision for ADR-0011.
   - The path search finds plain and UTF-16 text only; a compressed or encrypted state hides its paths. It is a warning, not a guarantee. Finding the named file by content (ADR-0008) and sending it, like a sample, is possible only when the path is visible.

9. **A version difference can break state sync silently.** Version 2 of the test VST3 saves a newer format, which version 1 refuses, as many plugins do. When C (version 2) changed its wavetable, A and B (version 1) loaded the state, nothing happened, and nothing told the host: JUCE's `setStateInformation` returns nothing and ignores the plugin's `setState` result. The host still knows, because it reads the state back: it classifies every load as loaded exactly, loaded but saved back differently with the same parameters (what version 2 did with version 1's state), loaded with different parameters (a silent difference), or not loaded at all, and logs a `WARNING` for the last two. So A and C went on sounding different, and both hosts could say so. For the open question in ADR-0010 (flag or block): state sync makes a version difference matter more than parameter sync did, but blocking would still refuse every shared plugin (#14). This suggests flagging at join, as leaned, plus a per-load warning when a state did not load or loaded differently, naming the versions. Real version pairs decide it; they are in the checklist.

10. **A truncated state** (the plugin's own state cut in half) was ignored by the test VST3. On Windows, 17 of 28 real plugins took one without complaint and changed their state (finding 11). That makes it necessary, not just safer, to check the hash before every load, which the spike does.

11. **Real plugins on Windows (the probe, 28 plugins, Windows 11, 15 November 2026).** Nothing crashed or hung, `--try-bad-state` included. In the table below, "state" is the component and controller states as raw bytes, which the product would send. The probe's total also includes JUCE's wrapper (finding 6).

    | Plugin | State (raw) | Compressed total | Load ms (median) | Hash comparison works |
    | --- | --- | --- | --- | --- |
    | Serum 2 2.0.24 | 1.2 KiB + 910 B | 2.2 KiB | 13 | yes |
    | Sylenth1 3.0.7.3 | 129.7 KiB | 129.8 KiB | 4 | yes |
    | Play 1.0.0 (Novation) | 59.3 KiB | 6.8 KiB | 25 | yes |
    | Ozone 9 Elements 9.13.0 | 5.1 KiB + 5.1 KiB | 5.3 KiB | 14 | yes |
    | Serum 1.3.6.8 and SerumFX | 6.1 KiB | 5.2 KiB | 217 to 232, up to 3.6 s | **no** |
    | 16 FabFilter plugins | 79 B to 5.1 KiB + up to 1.2 KiB | 179 B to 2.3 KiB | under 1 | yes, except Pro-Q 3 and 4 |

    - **State is small.** Serum 2's is about 2 KiB, so the issue's size question is answered for Windows. The largest is Sylenth1 at 173 KiB with JUCE's wrapper, and it hardly compresses. Every read took 7.3 ms or less, so polling while a window is open costs little for all of these.
    - **Comparing by hash works for most.** 23 of 28 plugins saved back exactly the bytes they were loaded with, in another copy whose parameters had been moved first. Serum 2, Sylenth1, Ozone and 14 FabFilter plugins are among them, so a knob change in them needs no state transfer (finding 3).
    - **Serum 1 and SerumFX 1.3.6.8 break every assumption.** Two reads in a row differ, so the host marks them noisy and does not poll them. Loading their own state does not save it back unchanged. Each load takes 0.2 to 3.6 s on the message thread. SerumFX also changed its state again within 2 s of a load. The probe does not say exactly when, so the 1.5 s settle window may not cover it. For these two, every announcement means a transfer, and only hints and the mouse can find a change.
    - **Five plugins load into another copy with different parameter values:** Serum 1, SerumFX, Pro-Q 3, Pro-Q 4 and VocalSynth 2. This is the silent difference in rule 2 that the host finds and sends on. The same five, plus Melodyne, also differ between two fresh copies, so for them a join always transfers a state.
    - **17 plugins take a truncated state** (Emergence and all 16 FabFilter plugins) and change, with no error. The other 11 refuse it and keep their state.
    - **No paths were found in any state.** That is with nothing loaded and no presets, so it says nothing about samplers or imported wavetables. That is still the checklist's job.
    - **No plugin called `setDirty`** while probing. That is expected with no edits; Serum 2 did restart 11 times. Whether real edits produce hints is still the checklist's job.

## Recommendation for PLG-6

Provisional until the probe has run on the Mac and the checklist on real plugins; [ADR-0011](../../docs/adr/0011-plugin-state-sync.md) (Proposed) records it. The Windows probe (finding 11) supports it and adds the last point.

- **Sync the plugin's saved state as a content-addressed snapshot**: the component and controller states as raw bytes, named by SHA-256 like samples (ADR-0008), announced by hash through the relay and fetched only by those whose plugin does not already save that hash. Large states travel like samples (`spikes/sample-transfer`): chunked, resumable, as binary frames, never in front of live parameter changes.
- **The newest state wins as a whole**, ordered with the session's other operations; parameters keep their own order and are reconciled after each load as in this spike. The announcement is an operation in the ordered log (so a late joiner gets the current state); the bytes are not.
- **Detect changes in layers**: plugin hints, the mouse let go in the plugin's window, and polling only while the window is open, budgeted by read cost, with noisy plugins excluded. Verify every load by reading it back, and warn when it did not take.
- **Treat file paths in a plugin's state as the plugin's business, and say so**: detect and warn when the other machine lacks a named file; do not promise to fix it.
- **Do not block on a version difference**; flag it, and warn per load when a state is refused or loads differently.
- Do the reads and loads where the interface does not stall: VST3 needs them on the main thread, so the product should avoid reading large states often (budgeted polling) and show progress when loading a large one.
- **Expect some plugins not to fit**, as Serum 1 did on Windows: different bytes on every read, a slow load, and changes after a load. Such a plugin still syncs (hints, the mouse, a full transfer every time), but it is not polled and cannot be checked by hash. The settle window may need to come from measuring each plugin, not a fixed 1.5 s.

## What is left (needs the maintainers' machines)

- [x] The probe on Windows (finding 11). Paste the full report into the issue.
- [ ] The probe on the Mac, pasted into the issue.
- [ ] The checklist above with Serum 2, another synth and an effect, on one machine or two.
- [ ] A plugin that names a file, with that file on one machine only.
- [ ] At least one version pair from #14, both directions.
- [ ] Then: fill the table, settle the recommendation, and move ADR-0011 and ADR-0010's flag-or-block question on.

## Not covered

- Plugin formats other than VST3 (ADR-0010).
- Sending files a state names. Only detected.
- **Omnisphere, and plugins that play from a large sample library on disk.** Not tested; to come back to in #117, which also covers sending the files a state names. They are where a file one machine lacks matters most (finding 8). The Windows machine has Omnisphere only as VST2, AAX and standalone, which the spike cannot host, and the Mac has none. To start: install its VST3 (the Spectrasonics installer or updater), run `--probe-state` on `Omnisphere.vst3`, then the checklist's file test with a patch whose samples only one machine has.
- The relay: the spike uses its own TCP link, with JSON and base64 where the product would use binary frames.
- Undo of a state change, and saving states in the session file (they would be operations, ADR-0004).
- Crashes on load (PLG-8), and loading out of process.
