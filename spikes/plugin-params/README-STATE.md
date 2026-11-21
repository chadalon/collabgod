# Spike: sync plugin state that is not a parameter

Phase 0 spike for [issue #13](https://github.com/chadalon/collabgod/issues/13) (PLG-6). Parameter sync ([issue #12](https://github.com/chadalon/collabgod/issues/12), [`README.md`](README.md)) gives two people the same knob values, but Serum 2 keeps its LFO shapes, modulation and wavetables outside its parameters, and Lounge Lizard changes its parameters on a preset load without telling the host. The only thing a host can do with that part of a plugin is save the plugin's whole state and load it somewhere else. This spike does that between members, measures what it costs, and tries how far a generic way of noticing that the state changed can go.

It builds on the parameter spike in the same folder, because state and parameters have to agree with each other: the same app, the same test VST3 (with a hidden part added), and the same TCP link. It is throwaway code. Product code never includes it.

## Where the issue stands

| Done when | Status |
| --- | --- |
| A preset loaded in one instance appears in the other by sending the plugin's saved state | **Works with the test VST3** (a hidden wavetable, a preset that moves parameters silently, large embedded data), three members on this machine. **Works with Serum 2** on Windows: presets, LFO shapes, modulation and wavetables follow within a second, but Serum 2 is silent for about 4 s after every load (finding 12). **Works with FabFilter Pro-Q 3**: presets, bands and even the display range follow at once, with no gap (finding 13), also between a Windows PC and a Mac with two people editing at the same moment (finding 16). An edit could be silently undone by a state read in the middle of it (finding 18, a spike bug): **fixed in the rules**, the band drag on the Mac still to repeat |
| A way to detect that non-parameter state changed, or a fallback such as periodic state hashing | **Built and measured**: hints from the plugin, the mouse let go in its window, and budgeted polling with hashing. How generic it can be: finding 2. Serum 2's own hints were enough (finding 12) |
| State size measured for Serum 2 and at least two other plugins | **Measured** on Windows for Serum 2 and 27 others (finding 11), and on the Mac for 18 plugins (finding 15) |
| Behaviour when a preset references a file the other machine lacks is documented | **Documented for the test VST3, and the host detects it** (finding 8). **Serum 2** keeps only a reference to an imported wavetable and opens its own Missing Files dialog; the host did not see the path (finding 12). Samplers and libraries: #117 |
| Recommendation written for PLG-6 | **Written** ([below](#recommendation-for-plg-6) and [ADR-0011](../../docs/adr/0011-plugin-state-sync.md), Proposed), with Serum 2's, Pro-Q 3's and Sylenth1's results, and the Mac and two machines (findings 15 to 18). Pro-L 2 on two machines and listening to the version pair newer to older are still to do |
| Plugin versions (the comment on the issue) | **The harness classifies every load** (finding 9); the test VST3 shows a silent refusal. **Pro-Q 3 3.2.7.0 and 3.2.9.0 sync both ways** and sounded the same older to newer; the classification gave a false "not loaded" between versions (finding 17), now fixed |

## How state sync works

The rules are in `src/core/StateSync.*`, plain C++ with no JUCE, beside the parameter rules and on the same Lamport clock.

1. **The hidden part of a plugin is one value.** The newest announced state wins as a whole, ordered by stamp. There is no merging inside a state. A state from someone else that arrives while a change here has not gone out yet replaces that change: it is loaded once nobody here holds a control, and any parameter changed here since its sender read it, the held one included, is set back on top. So a person keeps the knob in their hand and everything else follows the other person's preset; a hidden edit here that had not gone out (an LFO shape) is lost to it ([the decision](#decision-a-knob-turned-while-someone-elses-state-arrives)). This copy never answers with its own whole state. Two states announced at the same moment, each before its sender saw the other's, are ordered by stamp. Nothing assumes two people.
2. **Parameters keep their own order.** A state travels with its sender's parameter values and stamps from when it was read. After loading it, a parameter someone changed since then is set back to the newer value. A parameter the loaded state holds differently from its sender's snapshot was changed by the plugin without telling the host: it counts as written at the state's stamp and is sent on as a parameter change, so the sender's host learns it too.
3. **Changes are found by reading.** The host reads the state (`getStateInformation`) and hashes it when something hints that it changed: the plugin calls VST3 `setDirty`, restarts or changes program, floods the host with parameter reports that change nothing (what Serum 2 does while an LFO is edited), or the person lets go of the mouse in the plugin's window. Polling can be added, all the time or only while the window is open; it slows itself down so reads take at most 2 % of the time.
4. **A change is announced once it is still.** The state must read the same for 300 ms, with no parameter moved here for 100 ms and no control held, and the parameter snapshot that goes with it is taken at that read. A control someone else is holding does not hold it back (see the decision under rule 1). A burst of edits becomes one announcement.
5. **An announcement carries the hash, not the bytes.** A member whose plugin already saves that hash fetches nothing: the change was only parameters, which parameter sync carried. Otherwise it reads once more after 150 ms (a value set on a plugin reaches its saved state only when it next processes audio) and then asks for the bytes.
6. **The bytes go only to whoever asks**, zlib-compressed, in chunks, paced so parameter messages are not stuck behind them. The receiver checks the SHA-256, waits while someone here holds a control, then loads them.
7. **Loading never echoes.** For 1.5 s after a load (or two polling intervals, if longer), changes in the state are the plugin settling, not news, unless a person does something here first: moves a parameter, or lets go of the mouse in the plugin's window. A host that cannot see the mouse in the window (`StateOptions::learnSettling`) learns per plugin whether it ever rewrites its state after a load, and after three loads without a rewrite drops the window for it, so an edit just after a load is not taken for settling; a plugin seen rewriting keeps it. The app sees the mouse, so it keeps the window and needs no learning. A change seen straight after parameters arrived from someone else, with nothing done here, is those parameters; a hint from the plugin counts as an answer to them only if it came within 50 ms after them, since the host notes their arrival before it sets them and hints reach it later. A plugin whose state differs on every read with nothing done is marked noisy and is not polled (changes that follow others' parameters do not count); its changes go out from the first read after nothing has been done for the quiet period. If a load put an older value into a parameter and the newer one was set back, the plugin reporting the older value later is an echo, and the newer one is set again: the first such report within 30 s, once.
8. **A member who joins** is told the agreed state, under its stamp, by everyone who has it. An untouched state has stamp 0, so any edited one wins over it.
9. **Everyone sees where the others' hands are, and nothing is locked.** Each copy tells the others when its plugin window opens or closes and when a mouse button is pressed in it (`presence` messages), and the controls each member holds come with their parameter changes already. The app shows a line per other member: the plugin they have up, window open or closed, and the controls they are turning, or "pressing in its window" for something with no parameter (an LFO shape). As with two people at one synth, either of them can still do anything: loading a preset while the other turns a knob is their call, and rule 1 says what happens.

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

Press **Log step** in both copies once before starting and again after each item. Each press appends what changed since the last one (the counters that moved, with their change, the state lines and the log) to a `steps-<plugin>-<member>-<time>.md` file per copy in the report folder, so the counters need not be read out. **Changed only**, beside the counters' filter, shows the same counters on screen; the counters stop refreshing while some of their text is selected, so it can be copied.

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
| Sylenth1 | 3.0.7.3, Windows 11, two copies on one machine | 173 KiB, 130 KiB compressed | yes; about 3 s, found by polling after an early read; no gap | not applicable | restart / program, mouse, polling | not applicable | not tried | none with exact values | does not report a preset's parameters; the loader finds and sends them (finding 14) |
| FabFilter Pro-Q 3 (effect) | 3.2.7.0, Windows 11, two copies on one machine | 2.1 KiB, 0.3 KiB compressed | yes, at once, no gap | bands and display range yes | mouse let go, polling; `setDirty` twice in all | not applicable | not tried | none; edits a second apart in both copies settled on one state | every gesture announces a state, which matches by hash with exact values; loads take 0.1 ms; view settings travel (finding 13) |
| FabFilter Pro-Q 3 (effect), two machines | 3.2.7.0 on Windows 11 and 3.2.9.0 on macOS 13.7, two people on a LAN | 2.2 KiB, 0.3 to 0.4 KiB compressed | yes; fetched in 5 to 15 ms, loaded in under 1 ms | bands yes | Pro-Q 3's own (`setDirty`, unchanged reports, restart); the mouse on Windows only | not applicable | both ways: newer into older loads exactly; older into newer saves back differently, parameters match; sounded the same older to newer | none heard; one state went back to the sender 7 s after a race and settled | different bands at once: both kept; same band: last to let go wins; presets at once: one wins whole. **A drag was undone** by a stale state (finding 18) |
| Serum 2 | 2.0.24, Windows 11, two copies on one machine | 6.5 KiB (4.7 KiB + 1.8 KiB) with a factory preset; 6.5 KiB compressed with JUCE's wrapper | yes, under 1 s; then silent about 4 s | all yes | setDirty (35 to 108 per LFO drag), restart, unchanged reports, mouse; hints alone enough | reference only: Serum 2's Missing Files dialog; host saw no path | not tried | none; one announcement per change | every load silences it about 4 s; a turned knob transferred a state only because of the spike's rounding (finding 14); once showed two LFO shapes with equal states (finding 12) |

## Findings

Findings 1 to 10 are from a cloud session on Linux (2 cores) with the test VST3 and the rules' random test. Finding 11 is the probe on a maintainer's Windows machine with real plugins, and findings 12 to 14 the checklist by hand with Serum 2, FabFilter Pro-Q 3 and Sylenth1 on the same machine. Finding 14 also corrects 12 and 13: a bug in the spike made every knob turn load a state. Finding 15 is the probe on a maintainer's Mac, and findings 16 to 18 are Pro-Q 3 between that Mac and the Windows PC, with two people editing at once and two versions of the plugin.

1. **A saved state carries what parameters do not.** A new wavetable in A's plugin, which no parameter describes, reached B as A's saved state and B sounded the same: every one of the first eight harmonics within 0.0001 of A's, where before the change they differed by up to 0.45. From the change to B playing it took about 0.5 s, nearly all of it on purpose: 30 ms for the hint, 300 ms for the change to be still, 150 ms for B to read again. The transfer itself, 266 bytes, took 6 ms.

2. **A generic detector exists, and its limits are cost and certainty, not reach.** The issue's comment asks how feasible generic detection is. Reading the state and hashing it finds every change, from any plugin, with no help from it: polling found the test VST3's change with every hint off in about 0.8 s at a 500 ms interval. What it costs:
   - **Reads are on the message thread** (VST3 requires it; JUCE takes the message manager lock), and they scale with the state: 0.07 ms for 266 bytes, about 20 ms for 1.3 MiB, 120 to 150 ms for 10.7 MiB on this machine. A 150 ms read is a visible hitch in the interface. Polling is budgeted to 2 % of the time, so a 10 MiB state is read about every 7 seconds; a 266 byte one every 500 ms.
   - **A hash says something changed, not what.** Every knob move changes it too. Finding 3 is how that costs nothing.
   - **Some states change with nothing done** (a counter, a time). Two reads in a row at load time, and four unprompted changes in a row later, mark such a plugin noisy, and it is then not polled. The self-test's noisy plugin was caught and sent nothing.
   - **Polling cannot tell a person's edit from the plugin settling after a load.** With no signal at all, the random test first lost an edit in 2.5 to 7 % of runs (2,000 runs each for 2, 3 and 4 members). Most of that was a rules bug, not this limit: parameters arriving from others counted as the plugin changing by itself, so busy members were wrongly marked noisy and stopped polling (finding 18). With that fixed, no run of 2,000 lost an edit. Since the decision on a knob turned during someone else's state (incoming states now always load, so they load more often), an edit made in the 1.5 s after one arrives, with no sign of a person, is again sometimes taken as settling: 2.5 to 6 % of runs lost one, with only `setDirty` or with no signal at all. With the mouse seen in the window or a parameter moved here, still none.

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
    - **No plugin called `setDirty`** while probing. That is expected with no edits; Serum 2 did restart 11 times. Edited by hand, Serum 2 calls it constantly (finding 12).

12. **Serum 2 by hand (two copies on one Windows 11 machine, 19 November 2026).** Serum 2.0.24, the checklist above, with **Hints, and poll while the window is open** unless noted. The two saved reports are pasted into the issue.
    - **Everything hidden follows.** A factory preset, an LFO shape, an LFO dragged onto a knob and a factory wavetable each reached the other copy in under a second. Each state was 8.5 to 8.8 KiB with JUCE's wrapper (component 4.7 KiB and controller 1.8 KiB as raw bytes), 6.3 to 6.5 KiB compressed, and took 3 to 10 ms to arrive from asking; one took 2.7 s, unexplained.
    - **Every load silences Serum 2 for about 4 seconds.** The host's load took 150 to 330 ms; the sound came back about 4 s later, every time, for a preset or a single LFO point. This is the biggest finding: each state Serum 2 receives costs the receiver 4 s of silence, so the product must load as few as it can. **Only the receiver goes silent** (the maintainer, 20 November): picking the preset in Serum 2's own browser causes no gap on the copy where it was chosen. The gap comes from loading the plugin's whole saved state (`setStateInformation`), which is not what Serum does for its own presets. NFR-4 now says another member's preset load must sound no worse to you than it did to them (PR #135), so this is a gap in PLG-6 as built, not only a cost. Ways to close it are not tried: load the state into a second instance in the background and switch over when it sounds, or crossfade between the old and new instance.
    - **A turned knob sent a state, because of a bug in the spike (fixed, finding 14).** Parameter sync carried the knob, but the receiver's state did not then hash the same as the sender's, so it fetched and loaded it, with the 4 s gap. An on/off switch did match (`already had it`). Across both reports, 1 of 103 states received was already there. The cause was the spike's wire format, which rounded parameter values to 7 decimal places, so a turned knob arrived a hair off. With values sent exactly, a knob turn in Serum 2 matched by hash, loaded nothing and gave no gap. A preset still loads, with the gap.
    - **Serum 2 hints, a lot.** One LFO drag raised `setDirty` by 108 and the unchanged parameter reports by 19. With detection set to hints only, the LFO edit still synced. Both copies also called restart about 90 times over the session. In both, the host marked the state noisy (it changed with nothing done) and stopped polling it, so hints and the mouse found the changes.
    - **An imported wavetable is a reference.** With the file's folder renamed, the receiving Serum 2 opened its own **Missing Files** dialog, naming the oscillator and the file, with folders to search and a **Start search** button. The host found no path in the state: Serum 2 stores it in a form the text search cannot see. So the warning in finding 8 does not work for Serum 2, and the plugin tells the person itself. Serum 2 also wrote `can't open C:\Users\<name>\Documents\wt-test\<file>.wav: 2` (2 is "file not found") to the console, many times: it opens the file by the sender's full path, so the operating system sees the path even though the host cannot, and a file put at that same path would be found only if the user names match. Finding the file to send is #117.
    - **No echo.** One LFO change gave one announcement in each copy, and nothing went back and forth.
    - **Both at once.** Presets changed in both copies together looked the same at the end, but the log shows it was rough. In one burst of six loads in 4 s, Serum 2 refused three (`not loaded: the plugin's state did not change`), probably because they arrived while it was still busy with the one before. Eight loads gave parameter values different from the sender's (1 to 9 of 541), which the host found and sent on (24 parameters). And two states bounced: each copy loaded the other's, saved it back slightly differently and announced again, three times in 10 s, and again later; it stopped by itself. The host saw all of it, but it does not retry a refused load, so a refusal can leave two copies apart. Quick LFO edits in both did once leave the two copies drawing different LFO 2 shapes: they announced with the same stamp, the tie was broken one way, and the loser loaded the winner's state and read it back exactly (`loaded exactly`). Reopening the plugin windows and reading the state again changed nothing. So the two states were equal and Serum 2 showed different shapes: inside the plugin, or a part of what it plays that it does not save. Not reproduced.
    - **A stuck sound.** After those edits, both copies played a plain init-like saw while showing a preset; reopening the window did not help, and loading a factory preset in one copy fixed both. The preset used the imported wavetable whose file was missing, and Serum 2 kept failing to open it (above), which may be why; not checked.
    - **All of this ran before the rounding fix (finding 14).** Some loads in "Both at once", and the states that bounced, may have come from parameter values arriving a hair off rather than from Serum 2. Only the knob turn was rerun.
    - **Not tried:** a sampler (Serum 2's noise oscillator with a user file behaved oddly and was left for #117), another version of Serum 2, and a second synth and an effect by hand.

13. **FabFilter Pro-Q 3 by hand (two copies on one Windows 11 machine, 19 November 2026).** Pro-Q 3 3.2.7.0 on a tone, **Hints, and poll while the window is open**, with **Log step** pressed in both copies after each step. The two saved reports are pasted into the issue.
    - **Everything follows, with no gap.** A factory preset, a band added by double-clicking, a band dragged, and a band's gain moved from the app's parameter list all showed in the other copy at once. The state was 2.1 KiB (0.3 KiB compressed), arrived 2 to 8 ms after asking and loaded in 0.1 ms, and every load read back `loaded exactly`. The tone never dropped. Muting each copy in turn, the two sounded the same.
    - **Every gesture announces a state.** A band drag sent about 90 parameter changes and then, when the mouse was let go, announced one state. The slider change, with the mouse never in Pro-Q's window, was found by polling (no hint) and also announced one. In this run the receiver's hash differed although it had the parameters, so it loaded the state; 6 of the 34 states received were already there. That was the spike's rounding bug (finding 14): rerun with exact values, a band added and a band dragged each matched by hash, with nothing transferred. The loads were inaudible anyway.
    - **View settings travel too.** Changing the display's dB range changed no parameter, sent a state, and changed the other copy's display. Two people cannot keep different views of the same Pro-Q: the host cannot separate what is view from what is sound in an opaque state.
    - **Hints are few.** Pro-Q 3 called `setDirty` twice in the whole session. The mouse let go in the window found the edits made there, and polling found the one made from outside.
    - **Edits close together.** With one mouse, edits alternated between the two copies about a second apart, 12 in 45 s. Each copy loaded the other's state each time, all exactly, except 4 that were already there by hash, and the two ended on the same state, with nothing bouncing after. At the start one copy announced two states within a second, and both loaded with 2 of 346 parameters different from the sender's, which the host found and sent on. The person saw a brief glitch then, and then everything in step. Two people editing at the same moment was not tried; it needs two machines.
    - **Both copies were marked noisy.** Each saw its state change "with no hint or activity" a few times (7 at most), soon after the other's parameter changes arrived, and then stopped polling. That was a rules bug, found and fixed with finding 18: a change seen after parameters arrived from someone else counted toward "changes by itself", so a busy session made any plugin look noisy. Such changes no longer count. Once a copy was marked noisy, a change made only from outside the window (automation, the host's own controls) was no longer found by polling, and a noisy copy announced stale bytes (finding 18).

14. **Sylenth1 by hand, and a rounding bug in the spike (same machine, 19 November 2026).** Sylenth1 3.0.7.3, playing the looped chord, with **Log step** after each step.
    - **The bug.** The spike sent parameter values as text with 7 decimal places, which does not bring every float back exactly; 9 significant digits do. A turned knob therefore arrived a hair off, the receiver's state hashed differently from the sender's, and it fetched and loaded the whole state. A switch (exactly 0 or 1) survived, which is why switches matched and knobs did not in Serum 2 (finding 12). After the fix, a knob turn matched by hash in Sylenth1, Serum 2 and Pro-Q 3: no transfer, no load, and no gap in Serum 2. The self-test now checks that 1,000 values arrive bit for bit. The product's wire format must keep values exact for the same reason.
    - **Presets follow; Sylenth1 does not report a preset's parameters.** A preset showed and played the same in the other copy. The state is 173 KiB (130 KiB compressed, the largest so far); it arrived in about 22 ms and loaded in 5 ms with no gap. Sylenth1 changed about 124 parameters on a preset load but reported 1 to the host, so only the state carried the preset. The copy that loaded it found the rest (`parameters it changed silently`, 20 of 246 different from the sender's snapshot) and sent them on, as designed.
    - **The first read after a hint can be too early.** After a preset change Sylenth1 hinted (program changed) at once, but the state read then still held the old preset, so that announcement matched the other copy and did nothing. The new state was found about 3 s later by polling, with no hint, and only then followed. So a preset took about 3 s to follow, all of it detection. Reading again a second or so after a hint would close it; not built.
    - **A state came back once, before the fix.** In one run before the fix, after each preset the receiving copy's state changed a few seconds later with no hint, it announced it, and the sender loaded it back (twice in 20 s, then it stopped). It did not happen again with exact values, so it was probably the rounding too, but that was not proved.
    - **Restarts heard were the test loop.** The spike plays an 8 s chord on a loop, so a note-triggered LFO or arpeggiator restarts every 8 s on each copy, at different moments. An LFO restart first put down to a state load was this.

15. **Real plugins on the Mac (the probe, 18 plugins, macOS 13.7.2, 19 November 2026).** Nothing crashed or hung, `--try-bad-state` included. The full report is in the issue.

    | Plugin | State (raw) | Compressed total | Load ms | Hash comparison works |
    | --- | --- | --- | --- | --- |
    | Melodyne 4.1.0 | 13.0 KiB | 12.7 KiB | 7 | yes, but two fresh copies differ |
    | Ozone 9 Elements 9.0.3 | 5.1 KiB + 5.1 KiB | 5.3 KiB | 58 to 90 | yes |
    | VocalSynth 2 2.01 | 3.7 KiB + 3.7 KiB | 3.9 KiB | 19 to 20 | **no** |
    | Pro-Q 3 3.2.9.0 | 1.4 KiB + 96 B | 334 B | under 1.1 | **no** |
    | 8 other FabFilter plugins, Red 2 EQ, Red 3 Compressor, SPAN | 92 B to 3.6 KiB + up to 16 B | 168 B to 1.1 KiB | under 6.1 | yes |
    | Percolate, MSW2, Nebula (Soundspot) | **0 B** + 124 to 324 B | 112 to 185 B | under 0.1 | yes |

    - **It agrees with Windows.** The same two plugins load into another copy with different parameters (Pro-Q 3, now in a newer version, and VocalSynth 2), and Melodyne's fresh copies differ again. Hash comparison works for the other 16.
    - **Nothing is noisy and nothing is slow.** Every plugin read the same bytes twice and none changed after a load; the slowest read was Ozone's at 22 ms. Nothing here behaves like Serum 1 on Windows.
    - **Three plugins keep everything in the controller state.** The Soundspot plugins save an empty component state. Sending only the component state, as a host might assume is enough, would sync nothing for them: both states must travel.
    - **No `setDirty` and no paths**, as on Windows. 11 plugins take a truncated state (9 FabFilter, the two Red plugins) and 4 refuse it.

16. **Two people on two machines, Pro-Q 3 (19 November 2026).** Pro-Q 3 3.2.7.0 on the Windows PC, which hosted, and 3.2.9.0 on the Mac, which joined over the LAN; one person at each, counting down to act together; **Log step** in both after each step. States were 2.2 KiB (0.3 to 0.4 KiB compressed), fetched in 5 to 15 ms (up to 270 ms while the other copy was busy dragging) and loaded in under 1 ms, with no audible gap.
    - **Different bands at the same moment: both kept.** Each drag went over as parameters (225 and 185 changes sent), and both copies ended with both bands where their people left them. The state announced afterwards was already there by hash, or loaded with the same result.
    - **The same band at the same moment: the last to let go wins.** While both held it, each copy ignored the other's values (`ignored: held here`, 478 and 550), so each person saw the band follow only their own hand. When both let go, the band jumped on one screen to the other's final position, and both ended the same. The loser's drag is gone, but they saw it go: that is the rule, not a loss.
    - **Presets at the same moment: one wins whole.** Both copies found their own preset change and announced it; the copy whose change was still waiting when the other's arrived announced at once with a newer stamp, and the other loaded it. Both showed the same preset, and nothing flickered. In one race the PC loaded the Mac's state, went back to its own preset 7 s later (its own newer parameters set back on the load, or a second click; the logs cannot say), announced that, and the Mac loaded it: a short bounce that ended with both the same.
    - **Every load was found by the plugin's own hints on the Mac.** The "mouse let go in the window" hint counted 0 on the Mac in every step, with the plugin window open and in use; on the PC it counted 18. The Mac found changes through Pro-Q 3's `setDirty`, restarts and unchanged parameter reports. **Cause found (20 November):** the app asked JUCE (`ModifierKeys::getCurrentModifiersRealtime`), which on Windows and Linux asks the system, but on macOS knows about mouse buttons only from events that reach JUCE's own windows. A plugin's editor is a native view that takes its own clicks, so a press in it was never seen. The app now asks AppKit (`[NSEvent pressedMouseButtons]`, `src/app/MouseButtons_mac.mm`) on macOS. CI builds it on the Mac, but nobody has pressed a mouse there yet: to check, open Pro-Q 3's window, drag a band, and **Log step**; "mouse let go in the window" should rise by 1.
    - **Both copies were marked noisy** during the session, as in findings 13 and 14, while the probe found Pro-Q 3's state still when left alone. That was the rules bug fixed with finding 18.

17. **A version pair: Pro-Q 3 3.2.7.0 and 3.2.9.0 (#14).** The versions differ, and state sync works both ways.
    - **Older into newer** (the Mac loading the PC's states): 4 loads `loaded, saved back differently, parameters match`. The preset, its bands and its name showed in the newer version, including a preset the Mac's own list does not have (the name is in the state), and the two sounded the same with each muted in turn. Pro-Q 3 showed no error.
    - **Newer into older** (the PC loading the Mac's states): all 8 `loaded exactly`, the older version saving back the newer's bytes. Three of these loaded with 2 of 346 parameters different from the sender's; that is finding 18. Whether these sound the same has not been listened to yet.
    - **A false "not loaded".** Two loads into the newer version were classified `not loaded: the plugin's state did not change`, with a `WARNING`, although the person saw the preset arrive and no parameter differed from the sender's. Pro-Q 3 reports a preset's parameters, so parameter sync had already put the preset into the Mac's copy; the newer version saves it in other bytes, so the hashes differed, the Mac fetched the state, and loading it changed nothing. The spike checks "state unchanged" before it checks parameters. Between versions this happens whenever the parameters win the race, so the check must tell "already held this" (state unchanged, every parameter equal to the sender's) from a refusal, or it warns about every preset. **Fixed (20 November):** that case is now its own outcome, `nothing changed, parameters match the sender's`, logged without a warning; `not loaded` and its warning are kept for a state left unchanged with parameters that differ. The limit, shown by the test VST3's version refusal: a refusal of only a hidden part (a wavetable no parameter shows) looks exactly the same, and the host cannot tell them apart. Between versions, the flag at join has to cover that.
    - **For ADR-0010:** this pair needs a flag, not a block. A block would have refused a pair that works.

18. **A stale state undid an edit (a spike bug, found 19 November 2026, fixed in the rules 20 November; the band drag on the Mac is still to repeat).** On the Mac, a person dragged a band in Pro-Q 3 and it snapped back after they let go, on both machines. The events from both copies show how:
    1. During the drag, a hint from Pro-Q 3 made the Mac read its state. The bytes held the band halfway (two parameters at 0.2468 and 0.2880).
    2. The drag ended at 0.2476 and 0.0508. Both values went out as parameters, and the PC applied them.
    3. The Mac then announced the state it had read in step 1, with a parameter snapshot taken now, holding the final values. The Mac's copy was marked noisy, and a noisy copy skips the read that should confirm the state is still before announcing it (`StateSync::readDue` and `flush`).
    4. The PC loaded the state. Pro-Q 3 then held the halfway values, which differed from the snapshot, so rule 2 took them as changed silently by the plugin, stamped them with the state's stamp, newer than the end of the drag, and sent them on.
    5. The Mac applied them, and both copies ended on the halfway position. The person's last edit was lost on both machines, and the logs showed only "2 of 346 parameters differ from the sender's".

    The PC loaded with 2 parameters different 2 more times that evening, during band drags on the Mac, and one earlier snap-back was seen; the events were not being logged then. Pro-Q 3's own load difference (finding 11) is not the cause here: the values sent back were ones the Mac's band passed through. **The fix.** A change now goes out only once a read finds it with no parameter moved here for the quiet period, and the parameter snapshot is taken at that same read, for a noisy copy too: a noisy copy announces the first read after the quiet period, not bytes it read earlier. A newer state from someone else no longer makes an unconfirmed change go out early either. A rules test replays the drag and fails on the old rules. The cause of the noisy marking was a second bug: a change seen after parameters arrived from someone else counted as the plugin changing by itself, so busy sessions marked plugins noisy (findings 13, 14 and 16). It no longer counts. The random test gained a setting with noisy plugins (every save differs), which passes 2,000 runs for 2 to 4 members; it does not catch the old bug by itself, because the old rules converged on the halfway value, the unit test does. **Not built:** the suggested limit on rule 2 (a loaded state's value never wins over its sender's newer change to the same parameter). It would also stop a real silent change from being found whenever the sender had touched that parameter before, which is common (Sylenth1, finding 14), and with bytes and snapshot read together the stale value no longer arises.

    The event log (**Log events to CSV**) is written out only when it is switched off, so a file copied while logging was on ended mid-line and missed these events. Switching it off gave the full files.

## Recommendation for PLG-6

[ADR-0011](../../docs/adr/0011-plugin-state-sync.md) (Proposed) records it. The Windows probe (finding 11) and Serum 2, Pro-Q 3 and Sylenth1 by hand (findings 12 to 14) support it and add the four points from "Expect some plugins not to fit" to "Do not load in a burst". The Mac probe and Pro-Q 3 on two machines, with two people and two versions (findings 15 to 18), add the last three. Pro-L 2 on two machines, listening to the version pair newer to older, and the fix for finding 18 are still to do.

- **Sync the plugin's saved state as a content-addressed snapshot**: the component and controller states as raw bytes, named by SHA-256 like samples (ADR-0008), announced by hash through the relay and fetched only by those whose plugin does not already save that hash. Large states travel like samples (`spikes/sample-transfer`): chunked, resumable, as binary frames, never in front of live parameter changes.
- **The newest state wins as a whole**, ordered with the session's other operations; parameters keep their own order and are reconciled after each load as in this spike. The announcement is an operation in the ordered log (so a late joiner gets the current state); the bytes are not.
- **Detect changes in layers**: plugin hints, the mouse let go in the plugin's window, and polling only while the window is open, budgeted by read cost, with noisy plugins excluded. Verify every load by reading it back, and warn when it did not take.
- **Treat file paths in a plugin's state as the plugin's business, and say so**: detect and warn when the other machine lacks a named file; do not promise to fix it. Keep the path search, which is cheap, but expect it to miss: Serum 2's paths are hidden and the plugin warns itself. Sending the files is #117.
- **Do not block on a version difference**; flag it, and warn per load when a state is refused or loads differently. Pro-Q 3 3.2.7.0 and 3.2.9.0 synced both ways (finding 17); a block would have refused them.
- Do the reads and loads where the interface does not stall: VST3 needs them on the main thread, so the product should avoid reading large states often (budgeted polling) and show progress when loading a large one.
- **Expect some plugins not to fit**, as Serum 1 did on Windows: different bytes on every read, a slow load, and changes after a load. Such a plugin still syncs (hints, the mouse, a full transfer every time), but it is not polled and cannot be checked by hash. The settle window may need to come from measuring each plugin, not a fixed 1.5 s.
- **Send parameter values exactly, so that hash comparison does its job.** A load can cost the receiver seconds of silence (Serum 2: about 4 s), so a change that parameters already carried must not cause one. With values sent bit for bit, a knob turn left the two copies saving the same bytes in Serum 2, Pro-Q 3 and Sylenth1, and nothing loaded (finding 14). Rounded values made every knob turn load a state. Any format the product uses for parameter values (JSON, binary, or a relay that re-encodes them) must bring them back exactly.
- **Read the state again shortly after a hint.** Sylenth1 hinted before its state held the new preset, so the first read found nothing and the preset followed only when polling found it, about 3 s later (finding 14).
- **Do not load in a burst.** Quick edits in Serum 2 sent 12 states in 10 s (before the rounding fix), each would silence the receiver, and in another burst Serum 2 refused half the loads. When a load is refused, load the newest state again once the plugin has settled, rather than leave the copies apart. Waiting longer before announcing, or before loading, for plugins whose loads are disruptive trades a slower follow for fewer gaps.
- **Announce bytes and parameters read together, after the parameters are still.** A state read halfway through a drag and announced with the parameters from after it undid the drag on both machines (finding 18). That must hold for every plugin, including one marked noisy. And do not count changes that follow someone else's parameters as a plugin changing by itself.
- **Tell "already held this" from a refusal, as far as the host can.** Between plugin versions, a state whose parameters arrived first changes nothing when loaded, and was reported as not loaded (finding 17). Unchanged state with every parameter equal to the sender's is not a refusal, as far as the host can tell; a refused hidden part looks the same, which the flag at join covers.
- **Send both the component and the controller state.** Three plugins on the Mac keep everything in the controller state (finding 15).

### Decision: a knob turned while someone else's state arrives

Decided by the maintainer on 20 November 2026: **if I am turning a knob in a plugin while another member changes the preset in the same plugin, my knob stays in my control and everything else follows their preset.**

How the spike does it (rule 1):

- The incoming state is fetched as usual, but it is not loaded while anyone on this copy holds a control. Nothing this copy changed is announced while it waits, even if polling has already seen this copy's state change from the knob turn alone. Before the decision, this copy answered with its whole state and a newer stamp, which replaced the other person's preset on both copies.
- When the person lets go, their final value goes out as a parameter change, the incoming state loads, and the held parameter, with any other parameter changed here since the sender read its state, is set back on top. Parameter sync already gave the other copy the knob value, so both copies end on the same bytes and nothing is announced back.
- The same applies when no control is held: a change here that has not gone out yet is replaced by the newcomer, and its parameters are set back on top.
- **The other copy must be able to announce while the knob keeps moving.** Before, a change waited for every parameter to be still, including ones moved from elsewhere, so B's preset could not go out until A let go, and then raced A's own. Now a control someone else is holding does not hold back a change here: B's state follows A's moving knob without restarting its quiet period, and goes out about 0.3 s after the preset. A parameter someone else is holding is left to parameter sync on every copy that loads the state (its newest known value is set back), because the bytes may be a block behind the hand.
- **A load's late effects are part of the load.** On a slow machine (GitHub's macOS runner), a plugin's reports and restarts after loading a 10 MiB state came 1 to 2 s late, after the settling window, and the copy that loaded it took them for an edit and announced its whole state back. With a value lagging in those bytes, that once undid the other copy's knob. Now values set back after a load extend the settling window (up to 10 s after the load), hints during it are not taken as edits, and a hint within 50 ms of values set from elsewhere is the plugin answering them.

Tested by two rules tests (`the decision on #13: A holds a knob while B loads a preset...`, which fails on the old rules, and `a change here not yet announced is replaced...`) and by step 7b of the self-test, where A keeps turning Level from the host for 3 s while B changes its wavetable. B's change goes out while A is still turning; A loads it only after letting go; A keeps 0.42, both copies get B's wavetable and save the same bytes, A announces nothing, and B loads nothing back. The rules test turns the knob continuously too, and fails if a held knob elsewhere still holds back B's announcement.

**Check it by ear** (two copies on one machine; one mouse cannot hold a knob in one copy and click in the other, so **Hold 8 s** in the app holds a control from the host for eight seconds, sweeping it slowly. The rules treat it like a knob held in the plugin's window):

1. Start two copies as in [`README.md`](README.md#1-start-two-copies): A hosts, B joins. Load the same plugin in both, leave **Sync state** on with **Hints, and poll while the window is open**, open both plugin windows, and press **Play** in both. Press **Log step** in both.
2. Load a factory preset (P1) in A and wait until B shows it. Choose a second preset (P2) that sounds clearly different.
3. In A's parameter list, select a parameter that P2 also sets: for Serum 2 a filter cutoff (type `cutoff` in the filter box), for Pro-Q 3 `Band 1 Gain` (P2 must use band 1). Note its value in A.
4. Tick **Mute this one** in B, so you hear A.
5. Press **Hold 8 s** in A. Within the next two seconds, load P2 in B's plugin window. Then listen and watch until the 8 seconds are over, and a few seconds more.
6. Press **Log step** in both. Untick B's mute and tick A's, then compare by switching.

What you should hear and see:

| | Copy A (holding) | Copy B (loaded P2) |
| --- | --- | --- |
| During the hold | Still P1, by name and sound. The held parameter keeps sweeping smoothly. No gap | P2 by name at once. Its copy of the held parameter follows A's sweep, not P2's own value |
| When the hold ends | Loads P2 (Serum 2 shows P2's name; Serum 2 goes silent for about 4 s while it loads, finding 12; Pro-Q 3 shows P2's bands with no gap). The held parameter stays at the sweep's end value | Nothing changes. It does not go back to P1 |
| Afterwards | P2 with A's value for the held parameter. Same as B by ear | The same |
| Counters, from **Log step** | `announced` did not rise during the step; `applied` rose by 1; `changes here replaced by someone else's` may rise by 1 | `announced` rose by 1; `applied` did not rise |

The old behaviour, for comparison: at the end of the hold, B went back to P1 (with A's value), and B's `applied` rose. Also try the same step with nothing held: load P3 in B while A is idle. A should follow at once.

**Known limits:**

- **A hidden edit in progress here is lost to the incoming state.** An LFO shape being dragged, or a wavetable picked a moment before the other person's preset arrives, is replaced by it, with no merging. Accepted.
- **Should the person be told?** Open, for the maintainer. The rules count it (`replacedHere`, shown in the app as "changes here replaced by someone else's") but nothing tells the person.
- **Two announcements crossing.** If this copy announced a knob-only state just before the other person's preset reached it, the two are ordered by stamp, and this copy's can win and replace the preset. That needs both announcements in flight at once, within the time a message takes between the copies, in the few hundred milliseconds after the knob is let go. Not seen in the tests.
- **Edits in the moments after a load** with no sign of a person can be taken as the plugin settling. In the app, which sees the mouse let go in the window on every platform now, that does not arise: a hidden edit needs the mouse, and letting go of it ends settling. For a host without that signal, 2,000 simulated runs each for 2, 3 and 4 members lost an edit in 2.7 to 5.2 % of runs with only `setDirty` or no signal; learning per plugin whether it rewrites after a load (rule 7), with it learned, brings that to 0.9 to 2.0 %. What is left is an edit within a few tens of milliseconds of a load or of someone else's values, which nothing without a person signal can tell apart.

## What is left (needs the maintainers' machines)

- [x] The probe on Windows (finding 11). Paste the full report into the issue.
- [x] The probe on the Mac (finding 15), pasted into the issue.
- [x] The checklist above with Serum 2 (finding 12).
- [x] The checklist with an effect: FabFilter Pro-Q 3 (finding 13).
- [x] The checklist with another synth: Sylenth1 (finding 14).
- [x] Two people editing the same plugin at the same moment, on two machines: Pro-Q 3 (finding 16).
- [ ] The same with Pro-L 2: the same knob, then presets.
- [x] Fix finding 18, with a rules test that reproduces it (20 November), and the false noisy marking behind it.
- [ ] Repeat a band drag on the Mac, with the PC connected, to show the edit stays and neither copy is marked noisy.
- [x] Stop warning "not loaded" when parameter sync had already brought the preset (finding 17, 20 November).
- [x] Find out why the mouse let go in the window is not seen on the Mac (finding 16): JUCE does not see clicks in a plugin's native view there; the app now asks AppKit.
- [ ] Check it on the Mac: drag a band in Pro-Q 3, **Log step**, and "mouse let go in the window" rises.
- [ ] Check presence by eye on two copies: open and close the plugin window in one, turn a knob, drag an LFO point; the other shows each within a moment.
- [x] A plugin that names a file, with that file missing: Serum 2's imported wavetable (finding 12). A sampler or library is #117.
- [x] At least one version pair from #14, both directions: Pro-Q 3 3.2.7.0 and 3.2.9.0 (finding 17).
- [ ] Listen to the pair newer to older: presets loaded on the Mac, each copy muted in turn.
- [ ] Check the decision on a knob held during someone else's preset by ear, with Serum 2 and Pro-Q 3 (steps under "Decision" above).
- [ ] Decide whether a person should be told when their hidden edit was replaced by someone else's state (open).
- [ ] Then: fill the table, settle the recommendation, and move ADR-0011 and ADR-0010's flag-or-block question on.

## Not covered

- Plugin formats other than VST3 (ADR-0010).
- Sending files a state names. Only detected, and only when the path is visible (#117).
- **Omnisphere, and plugins that play from a large sample library on disk.** Not tested; to come back to in #117, which also covers sending the files a state names. They are where a file one machine lacks matters most (finding 8). The Windows machine has Omnisphere only as VST2, AAX and standalone, which the spike cannot host, and the Mac has none. To start: install its VST3 (the Spectrasonics installer or updater), run `--probe-state` on `Omnisphere.vst3`, then the checklist's file test with a patch whose samples only one machine has.
- The relay: the spike uses its own TCP link, with JSON and base64 where the product would use binary frames.
- Undo of a state change, and saving states in the session file (they would be operations, ADR-0004).
- Crashes on load (PLG-8), and loading out of process.
