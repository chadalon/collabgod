# Spike: mirror VST3 parameter changes between two app instances

Phase 0 spike for [issue #12](https://github.com/chadalon/collabgod/issues/12) (PLG-4, PLG-5). Seeing each other's knob moves live is a headline feature, and it depends on how real plugins report parameter changes. This spike hosts a VST3 in Tracktion Engine in each instance, hears its parameters change, and mirrors the changes to the other instances so their plugin windows move and their sound changes.

It is throwaway code. Product code never includes it.

## What is here

| Path | What it holds |
| --- | --- |
| `src/core/ParamSync.*` | The mirroring rules, in plain C++ with no JUCE, engine or network: what to send, what to apply, echoes, conflicts, throttling |
| `tests/ParamSyncTests.cpp` | Unit tests for the rules, and a random test: 2 to 4 members editing the same parameters at once through a simulated relay, with plugins that round values and echo them late, must all end on the same value and go quiet |
| `src/host/PluginSession.*` | One member: an edit with one VST3, Tracktion Engine's listeners on its parameters, the rules in between |
| `src/host/Wire.*` | The messages, as JSON: a hello with the plugin and its parameter list, and batches of changes |
| `src/host/Link.*` | TCP between instances. One hosts and passes every message on to the others, as the relay would |
| `src/selftest/SelfTest.cpp` | Three members in one process over TCP on 127.0.0.1, with the test plugin, checked end to end. `ctest` runs it |
| `src/app/Main.cpp` | The window for trying real plugins between two machines |
| `test-plugin/TestSynth.cpp` | A small VST3 instrument, built as version 1 and version 2 (see below) |

## How the mirroring works

Each change travels as the plugin's own **parameter ID**, never its index: the VST3 `ParamID`, which JUCE gives as `HostedAudioProcessorParameter::getParameterID()`. A value is normalised, 0 to 1. The rules (`src/core/ParamSync.h`):

1. **Last writer wins, per parameter**, ordered by a Lamport stamp (counter, member id). Any number of members converge on the change with the highest stamp. Nothing assumes two people.
2. **A change that repeats the value already known is never sent.**
3. **Echoes.** Setting a remote value on the plugin makes the host call its own listener back at once; that call is swallowed. A plugin may also report the value back later, sometimes rounded. Within the echo window (250 ms) after remote values are applied, a change the plugin reports without a gesture with one of those values is its echo and is not sent; if it repeats an older one, the newest is set again. A different value is a real change and is sent, unless someone else is holding the control: then it is dropped, and the plugin is set to the agreed value when they let go.
4. **Two people on one control.** While someone holds a control (between the plugin's begin and end gesture), remote changes to it are not applied on their machine, so it never fights their hand. When they let go, their final value gets a stamp newer than everything they have seen, so it wins everywhere. **The last person to let go wins.** Each side shows who else is holding it.
5. **Throttle.** Outgoing changes are coalesced per parameter: only the latest value is kept, and it is sent at most once per interval (33 ms by default). A gesture's start and end go at once, and the end carries the final value.
6. **Automation is never sent.** Both members have the same curve, because a curve edit is a session operation; each plays its own copy. Tracktion Engine does not report automation playback as a change, and a change that arrives while automation plays the parameter (a plugin repeating the automated value) is ignored.
7. **Only parameters the plugin marks automatable are synced.** Tracktion Engine skips the others. Tracktion's own dry and wet levels are not synced either: in the product they belong to the session, not the plugin.
8. **The hello compares parameter lists.** When a member joins, each side compares the other's plugin, version and parameters by ID, and logs a `WARNING` for every difference: a different plugin or version, a parameter only one side has, a different name or number of steps. A change to an ID this plugin does not have is ignored and logged once.

## Build and run it

Prerequisites are those of [`spikes/engine-toolchain`](../engine-toolchain/README.md#build-it): CMake 3.22 or newer, Visual Studio 2022 on Windows, Xcode 15 or newer on macOS. The same pinned JUCE 9 and Tracktion Engine are downloaded during configure.

```sh
cd spikes/plugin-params
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

The build makes the app and two versions of the test VST3. It compiles Tracktion Engine once, about as long as the engine toolchain spike. `ctest` runs the rules' tests (a second) and the self-test (about 15 seconds). The self-test prints a `PASS` or `FAIL` line for every check and `RATE` and `ECHO` lines with the numbers below. It opens no audio device: a simulated sound card keeps the plugins processing. Logs and renders go to `build/selftest`, including a CSV of every event for each member.

To work on the rules alone, without the engine build:

```sh
cmake -S . -B build-rules -DPLUGIN_PARAMS_CORE_ONLY=ON
cmake --build build-rules
ctest --test-dir build-rules --output-on-failure
build-rules/param_sync_tests --seeds 2000      # the random test with ten times as many cases
```

## Try it with real plugins

This is the part of the issue a cloud session cannot do. It needs your plugins, your ears and your mouse. Allow an hour.

### 1. Start two copies

The app is `build/plugin_params_spike_artefacts/Release/CollabGod Plugin Params Spike` (`.exe` on Windows, `.app` on macOS). Two copies can run on one machine, or one on each of two machines on the same network.

| | One machine | Two machines |
| --- | --- | --- |
| Windows | Start the `.exe` twice | Start it on each; allow it through the firewall on the host |
| macOS | `open -n "build/plugin_params_spike_artefacts/Release/CollabGod Plugin Params Spike.app"` twice (`-n` allows a second copy) | Start it on each |

In the first copy press **Host** (port 52799). In the second, type the first's address (`127.0.0.1:52799` on one machine, its LAN address otherwise) and press **Join**. The status line says `1 other member(s)` once they see each other.

The same can be scripted: `--load <file.vst3> --host 52799 --play` for the first and `--load <file.vst3> --join 127.0.0.1:52799` for the second.

On one machine both copies play through the same output. Tick **Mute this one** on one of them to hear only the other. Windows with an ASIO driver may let only one copy open the device: use Windows Audio for this.

### 2. For each plugin

Load the same `.vst3` in both copies with **Load VST3...** (on macOS, choose the `.vst3` bundle). An instrument gets a held chord; an effect gets a tone in front of it. Press **Play**, then **Plugin window** in both.

Check each of these and note what happened:

- [ ] **Plugin window.** Turn a knob in one copy's plugin window. The same knob moves in the other copy's window, and its sound changes. Note any lag you can see or hear.
- [ ] **Host control.** Drag the same parameter's slider in this app's list in one copy. The plugin windows of both copies follow.
- [ ] **Automation.** Select a parameter, press **Add sweep automation** in *both* copies, and play. Both plugins move with the curve; the counters show nothing sent (`changes sent/s` stays at 0 unless you touch something).
- [ ] **Both at once.** Each person grabs the same knob in their own window at the same time and wiggles it. Each hears their own value while holding it, and the list shows `held by m-...` for the other. When both let go, both end on the value of whoever let go last. Nothing keeps moving on its own afterwards.
- [ ] **Fast sweep.** Select a parameter and press **Reset peaks**, then sweep a knob in the plugin window as fast as you can for a second or two. Then press **Host sweep 1 s** for comparison. Try the **Throttle** choices: off, 16, 33, 50 and 100 ms. Note the peaks of `plugin reports/s` and `changes sent/s`, and whether the other copy's sound steps or crackles at each throttle.
- [ ] **Echoes.** Watch `not sent: echo of a remote value` and `not sent: someone else holds it` in the receiving copy while the other sweeps. Then set **Echo window off** in both and sweep again: if `changes sent/s` in the receiving copy is no longer 0, the plugin echoes, and the window is what stops it.
- [ ] **Preset change.** Load a preset in one copy. Many parameters change at once; note how many messages it sends, and whether the other copy ends up sounding the same. (Non-parameter state, such as a wavetable, is issue #13; this only records what happens.)
- [ ] **Versions.** If the two machines have different versions of a plugin, load it in both: the log shows a `WARNING` for every difference in the parameter lists. Note them.

Then press **Save report** in each copy. It writes a Markdown report to `Documents/CollabGod Spike Reports/plugin-params` and copies it to the clipboard: paste it into the issue. Tick **Log events to CSV** before a test to get every event with a timestamp, in the same folder.

### Plugins

Serum 2 and at least two others, ideally of different kinds: a synth from another vendor, and an effect (for example an EQ or a reverb). Note per plugin, in the issue:

| Plugin | Version, OS | Parameters synced / reported | Window → other window | Host slider | Automation not sent | Both at once | Peak reports/s, sent/s at 33 ms | Echoes? | Preset load | Other |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CollabGod Test Synth (this repo) | 1.0.0 and 2.0.0, Linux (cloud) | 4 / 6 | yes, 153 reports/s while it moves itself | yes | yes | yes | 153, 23 | only when told to | not tested | version 2 warned about, matched by ID |
| Serum 2 | | | | | | | | | | |
| Lounge Lizard Session 4 (AAS) | 4, Windows 11, two copies on one machine | 23 / 2104 (the rest are likely MIDI controller placeholders, 16 × 128) | yes, smooth to the eye | yes | yes | the `held by` label shows; not tried by two people at once | 76, 24 (host sweep: 175, 28) | no late echoes: 0 sent by the receiving copy with the echo window off | not synced: the plugin reports no parameter changes when a preset loads | no crackle at any throttle; pitch and mod wheels not synced (MIDI, not parameters); at 100 ms the window showed 16 sent/s: the window counts rates over 250 ms and multiplies by 4, so a gesture's start and end (sent at once) plus two throttled values read as 16; the throttle held |
| FabFilter Pro-Q 2 (effect) | 2.2.2.0 on Windows 11 and 2.1.1.0 on macOS, two machines on the same Wi-Fi | 190 / 326 | yes, no noticeable lag | used (331 changes from the sliders) | yes, 0 sent on both machines while both played the sweep | yes, between the two machines: the last to let go won | 40, 36 (session mixed sweeps and presets); host sweep: 87, 28 | none: the receiving copy sent 0 with the echo window off, with either machine receiving | synced and sounds the same: the plugin signals "everything changed" and Tracktion re-reads all 190; unchanged values are dropped (773), the rest go in a few messages | different versions synced by ID: the only difference logged was the version |
| | | | | | | | | | | |

## Findings

Findings 1 to 11 come from a cloud session on Linux with the test plugin. Where the manual tests on Windows 11 and macOS confirmed or changed one, it says so; those were Lounge Lizard Session 4 (an instrument, two copies on one machine) and FabFilter Pro-Q 2 (an effect, between two machines on Wi-Fi), with the full results in the issue's comments. Findings 12 and 13 come from those tests. Serum 2 is still to come.

1. **It works end to end with a real VST3 in Tracktion Engine.** Three members in one process over TCP: a change made with A's host control reaches B, B's plugin reports the new value, and B's sound changes (Level 0.25 to 0.8: B's render went from -15.1 to -4.9 dBFS, A and B within 0.01 dB of each other). A change the plugin makes itself, reporting it exactly as a knob in its window would (begin gesture, values, end gesture, through VST3 `performEdit`), moves B's Pitch from 185 Hz to 715 Hz. A third member with version 2 of the plugin joins and is synced. **Confirmed with real plugins:** a knob turned in Lounge Lizard's or Pro-Q 2's window moves the same knob in the other copy, smoothly and with no noticeable lag, including between a Windows laptop and a Mac on Wi-Fi, and both sound the same.

2. **Where plugin changes come from.** A change in the plugin's window arrives through VST3 `beginEdit`/`performEdit`/`endEdit`, which JUCE turns into parameter listener calls. Tracktion Engine's `ExternalAutomatableParameter` passes gestures on at once but values through an `AsyncUpdater`, coalesced to one per message loop pass. Two consequences:
   - **The end of a gesture can overtake its last value.** The spike reads the plugin's final value when the gesture ends, so the end message carries it.
   - Coalescing barely reduces the rate on an idle message thread: 200 plugin reports in a one-second sweep reached the sync as 200 (151 a second over the 1.3 s measured).
   Every report in the test came on the message thread. A plugin may report from its audio thread instead (the window counts these as "off the message thread"); JUCE's listener runs there, so anything the product does in it must be lock-free.

3. **A value set by the host reaches a plugin's processor only when it processes audio.** JUCE's VST3 host hands parameter changes to the processor inside `process()`. With nothing processing, a plugin never saw them, and its own reaction (the test plugin's "move pitch" control) never happened. Tracktion Engine processes plugins while the transport is stopped once a playback context exists, so in the app this holds by default; the self-test runs a simulated sound card for it. On Linux, a JUCE plugin's own timers also need its window open (the host hands it a run loop only then). Linux is not a target, but it is where this spike was developed. On the Windows CI runner the test plugin's timers ran with no window, as expected.

4. **Parameters must be matched by ID, and Tracktion Engine keys VST3 parameters by index.** Tracktion uses the parameter's list index as its ID for VST3 (to keep old automation working), and it changes when a plugin update inserts a parameter. The test plugin's version 2 adds "Drive" at the front: every shared parameter moved one index (Level from 0 to 1) and kept its ID (`102865796`). The spike reads the VST3 `ParamID` from JUCE, sends only that, and logged these warnings on each side when version 2 joined:
   ```
   WARNING member-a: parameter lists differ from member-c: different version: 1.0.0 here, 2.0.0 for member-c
   WARNING member-a: parameter lists differ from member-c: different parameter count: 4 here, 5 for member-c
   WARNING member-a: parameter lists differ from member-c: parameter 95852938 "Drive" only for member-c
   WARNING member-a: parameter lists differ from member-c: 4 shared parameters are at a different index (harmless: they are matched by ID)
   ```
   C's changes to Drive were ignored by A and B, once logged. For the product: session data and automation must key plugin parameters by `ParamID` too, not by Tracktion's ID, or a plugin update on one machine scrambles them. JUCE-built plugins derive the `ParamID` from a hash of the string ID, so it survives reordering. A plugin that changes its `ParamID`s between versions cannot be matched at all; PLG-7's version check should show that. **With a real pair of versions,** Pro-Q 2 2.2.2.0 on Windows and 2.1.1.0 on the Mac, all 190 synced parameters matched by ID across versions and operating systems, and the only warning was the version. Together with #14's finding that every plugin the two machines share differs in version, this argues for flagging a version difference, not blocking on it; a missing or renamed parameter is what is worth a louder warning.

5. **What gets synced.** Tracktion Engine skips parameters the plugin marks not automatable, so they are never synced. That is right for meters and the test plugin's controls, but a plugin that marks a real setting not automatable will not mirror it; watch for that per plugin. JUCE-built plugins also expose a "Bypass" parameter, which is synced. JUCE-built plugins add 2,048 hidden "MIDI CC" parameters to a VST3 unless built with `JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0`. **Real plugins:** Lounge Lizard reports 2,104 parameters and only 23 are synced; the rest are most likely such MIDI controller placeholders (16 channels × 128 controllers is 2,048), not automatable, so Tracktion skips them, which is what we want. Pro-Q 2 syncs 190 of 326. No setting that should mirror was found missing in either. The class this all goes through, `ExternalAutomatableParameter`, is in a header Tracktion does not export; the spike includes it directly. The product either asks Tracktion to export it or listens to the JUCE parameters itself.

6. **Echoes are real and the window stops them.** With the test plugin told to repeat every host-set value rounded to two decimals, as some plugins do, a 100-step sweep by A gave:

   | | Values B's plugin reported back | Recognised as echoes | Changes B sent back |
   | --- | --- | --- | --- |
   | Echo window 250 ms | 16 | 15 | 1, the rounding of the last value once A let go |
   | Echo window off | 16 | 0 | 16 |

   With the window off it still converged, because rounding twice gives the same value, but it doubled the traffic during the sweep and briefly pulled A's value back. The synchronous echo of setting a remote value is swallowed separately (3 in the run). Whether real plugins echo is the per-plugin question. **Neither real plugin did:** with the echo window off, the receiving copy sent nothing back while the other swept, for Lounge Lizard and for Pro-Q 2 in both directions. The window stays as a guard for plugins that do.

7. **Two people on one control converge, and the last to let go wins.** In both orders: each heard their own value while holding it (0.29 and 0.61), each saw the other holding it, and both ended on the value of whoever let go last. The random test checks this for 2 to 4 members under random delays, rounding and late echoes: 200 runs for each of 15 settings in CI, and 2,000 each locally, all converged and went quiet. It found two real bugs while this was written, both now fixed and covered: the member holding a control was taken from the change's stamp instead of its sender, so a member passing on someone else's value appeared to hold the control; and a change dropped while someone else held a control could leave that plugin out of step with everyone if that person's last value equalled the one already known. **Confirmed between two machines** with Pro-Q 2: one person on each held the same knob, each kept their own value while holding, and both ended on the value of whoever let go last.

8. **Automation is played on both sides and never sent.** With the same curve on Pitch in both edits and both transports playing for two seconds, both plugins swept from 0.10 to 0.90 and nothing reached either sync or was sent. Tracktion Engine applies automation without calling the parameter listener. **Confirmed** with Lounge Lizard and with Pro-Q 2 on both machines: nothing was sent while both played the same curve.

9. **Message rate during a fast sweep.** Measured on the cloud machine; sizes are this spike's JSON, about 78 bytes for a message with one change.

   | Sweep | Throttle | Changes in | Changes sent | Messages a second | Bytes a second |
   | --- | --- | --- | --- | --- | --- |
   | The plugin moving itself, 200 steps in 1 s | 33 ms | 200 | 30 | 23 | 1,760 |
   | Host control, 200 steps in about 1 s | off | 201 | 184 | 150 | 11,700 |
   | | 16 ms | 201 | 53 | 43 | 3,370 |
   | | 33 ms | 201 | 30 | 25 | 1,910 |
   | | 50 ms | 201 | 21 | 17 | 1,340 |

   Rates are over the 1.2 to 1.3 s from the first value to the last message. Every throttled run ended on the sweep's exact last value. The Windows CI runner gave nearly the same numbers (33 ms: 31 changes sent of 201, 24.5 messages and 1.9 kB a second; 16 ms: 46 messages a second). The macOS CI runner ran every message-thread timer at about half speed or less: the plugin's 5 ms timer ticked about 37 times a second and a 5 ms host sweep took 2.5 s, so its rates are lower but the ratios hold. Whether that is the runner or macOS slowing a windowless app (App Nap) is worth a look on the Mac.

   **A real mouse is slower than the test plugin.** Sweeping Lounge Lizard's knob through its window as fast as possible peaked at about 80 reports a second (76 sent unthrottled, 36 at 16 ms, 24 at 33 ms, 16 at 50 ms). A 1 s host sweep sent 28 of 175 changes at 33 ms, and 28 of 87 on Pro-Q 2. The app's window counts rates over 250 ms and multiplies by 4, so its peaks move in steps of 4, and a gesture's start and end, which skip the throttle, can make a short window read high: 16 a second at 100 ms was two throttled values plus the start and end, not the throttle failing.

10. **Proposed throttling.** Per parameter, keep only the latest value and send it at most every 33 ms (30 a second), with gesture begin and end sent at once and the end carrying the final value; batch every parameter that changed in the same tick into one message. That is about 2 kB a second per moving control, small next to anything else in a lobby, and a knob moved by hand still looks continuous. The receiving side steps every 33 ms, so a plugin that does not smooth its own parameters will step audibly (engine toolchain finding 9). If Serum 2 or another plugin zippers at 33 ms, try 16 ms for that parameter before adding smoothing in the host. (Windows timers at 16 ms behaved as 16 ms on the CI runner.) **On real plugins** no stepping or crackle was heard on Lounge Lizard at any throttle, even 100 ms, and Pro-Q 2 at 33 ms showed no noticeable lag between two machines. Serum 2, with its wider modulation, is the remaining check.

11. **For the product.** Live changes travel through the relay as `direct` messages, outside the ordered log, because there are 30 a second and nobody needs the intermediate values later. The value a gesture ends on, and any change without a gesture, becomes an operation in the ordered log, so it is saved with the session and a late joiner gets it; the relay's order then replaces the Lamport stamp for what is saved. The rules here are small and pure, so they can live in the TypeScript sync client (ADR-0002) with the C++ side doing only what it must: swallowing its own synchronous echo, reading the final value at gesture end, and coalescing before the bridge. A hello that compares plugins and parameter lists by ID belongs in the PLG-2 inventory exchange (issue #14).

12. **Presets: parameter sync carries some and not others, depending on the plugin.** Pro-Q 2 announces a preset load by telling the host that all its values changed; Tracktion Engine then re-reads all 190 parameters, the unchanged ones are dropped as repeats (773 of them), and the rest go out in a few messages, so the other copy follows and sounds the same at no extra cost. Lounge Lizard changes its values on a preset load without reporting anything to the host (plugin reports stayed at 0), so the other copy does not follow. Parameter sync cannot fix that: presets, and other state, have to travel as the plugin's saved state (#13, #56). A preset load that does arrive as parameters is a burst of up to every parameter at once, which the throttle and the repeat check already handle.

13. **Performance MIDI is not a parameter.** Lounge Lizard's pitch and mod wheels are MIDI (pitch bend and controller 1), so parameter sync does not carry them. Whether live MIDI from one person's keyboard should be heard on the other's machine is a separate question from this spike.

## What is left (needs the maintainers' machines)

- [x] Build on Windows and on the Mac. CI passes on GitHub's Windows and macOS runners, and the app ran on both maintainer machines.
- [x] Two other plugins through the checklist: Lounge Lizard Session 4 and FabFilter Pro-Q 2 (an instrument and an effect).
- [ ] Serum 2 through the checklist, on Windows, with its row filled in above.
- [x] A real mouse sweep (Lounge Lizard: about 80 reports a second at most). Serum 2's is still to come.
- [x] Two machines: Windows and a Mac on Wi-Fi, with Pro-Q 2.
- [x] Two versions of a plugin: Pro-Q 2 2.2.2.0 against 2.1.1.0, all parameters matched by ID.

## Not covered

- State that is not a parameter: presets, wavetables, samples inside a plugin. That is issue #13. Preset loads were observed here (finding 12), not synced as state.
- Adding a plugin on one machine and loading it on the other (PLG-4's first half): the spike loads the same file on both by hand. Scanning and inventories are issue #14.
- The relay. The spike uses its own TCP link; the product sends through `spikes/relay` (finding 11).
- Plugin crashes (PLG-8), and hosting out of process.
