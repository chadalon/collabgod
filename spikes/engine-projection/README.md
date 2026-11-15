# Spike: drive Tracktion Engine from the TypeScript model

Phase 0 spike for [issue #11](https://github.com/chadalon/collabgod/issues/11). ADR-0002 makes the TypeScript session model the source of truth and the C++ engine a projection of it. This spike tests whether that holds for tracks and audio clips: whether edits in the model change what Tracktion Engine plays, whether the engine can be rebuilt from the model at any time, and whether edits during playback cause dropouts.

It is throwaway code. Product code never includes it. It builds on `spikes/session-model` (#9), whose `apply` makes every edit here, and on `spikes/engine-toolchain` (#7), whose pinned JUCE and Tracktion Engine it builds against.

## How it works

```
session model ──project──▶ engine document ──diff──▶ commands ──▶ engine process ──▶ Tracktion edit
 (TypeScript)    pure        (seconds, files)   pure      (JSON)    (C++, no logic)        │
                                    ▲                                                     │
                                    └─────────────────── readback ◀───────────────────────┘
```

- **`project`** (`ts/src/document.ts`) is a pure function from the model's state to an *engine document*: tracks in order, each with its clips in seconds and the local file for each sample hash. It is the only code that knows how the model maps onto the engine.
- **`diff`** (`ts/src/diff.ts`) gives the smallest ordered list of commands that turns one document into another. A clip move is one `clip.set` with only the fields that changed.
- **The engine** (`src/Commands.cpp`) carries out primitive commands on Tracktion's edit and reports what the edit holds, read from Tracktion's own objects. It decides nothing. The commands and the readback are in [`contract/README.md`](contract/README.md); both sides test against the same examples.
- **`Projector`** (`ts/src/projector.ts`) holds what it last sent. On each model change it sends the diff. If the engine refuses a command it reads back and sends whatever makes the engine match the model. `rebuildFromScratch` empties the engine and sends everything. `verify` compares the readback with the projection.

The engine runs as its own process (`engine_projection serve`) and takes one JSON request per line on stdin. In the app the same messages would cross the web view bridge measured in #8; stdio stands in for it so Node can drive the real engine in tests.

### Hearing a dropout without ears

The edit always has one track the model never touches: the **probe**, a steady 440 Hz sine panned hard left. Every track the model makes is panned hard right. A plugin on the master bus, the **click detector** (`src/ClickDetector.cpp`), watches the left channel on the audio thread. A clean sine changes smoothly, so a jump in its second difference (a skipped, repeated or restarted block) or 16 samples of silence (a dropped block) can only come from the engine. The largest step a clean 0.2 sine at 440 Hz can make is 0.0008; a click is anything over 0.02. The detector also records the output by edit time, so a test can check what played where on the timeline.

A real device drops out when a block is not ready in time, and the generated audio does not show it. So the simulated sound card times every block against its real-time budget (10 ms at 441 samples), and also measures the audio thread's own CPU time in the block. Wall time much larger than CPU time means the thread was waiting, not working. On a real device, the device's own dropout counter takes this role, where it has one.

## Build and run it

### Prerequisites

The same as `spikes/engine-toolchain` (Visual Studio 2022 or newer, or Xcode 15 or newer; CMake 3.22 or newer; Git), plus Node 22 or newer for the TypeScript side.

### Build and test

```sh
cd spikes/engine-projection
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure

cd ts
npm ci
npm run typecheck
npm test
```

`npm test` runs the end-to-end tests through the real engine only when `ENGINE_PROJECTION_BIN` points at the built binary:

| | Binary |
| --- | --- |
| Windows | `build\engine_projection_artefacts\Release\engine_projection.exe` |
| macOS and Linux | `build/engine_projection_artefacts/Release/engine_projection` |

```sh
ENGINE_PROJECTION_BIN=$PWD/../build/engine_projection_artefacts/Release/engine_projection npm test
```

In PowerShell: `$env:ENGINE_PROJECTION_BIN = "$PWD\..\build\engine_projection_artefacts\Release\engine_projection.exe"; npm test`.

The configure step downloads JUCE and Tracktion Engine again into this build folder, and the first build compiles them, so expect as long as engine-toolchain's cold build. Use `--parallel 2` on an 8 GB machine (engine-toolchain finding 11).

### Commands

| Command | What it does |
| --- | --- |
| `engine_projection selftest [--out dir]` | Without an audio device. A: every contract example gives its readback. B: clips moved, removed and added half a second before the playhead are heard where they now are, and only there. C: 8 s of random edits while looping, many right at the playhead; the probe must stay clean and the readback must match. D: everything removed and rebuilt in one batch while playing. E: the output analysis finds bleed, dips and overs in a made-up signal and none in a clean one; B also records the engine's real output and checks the left carries only the probe. `ctest` runs this. |
| `engine_projection bench [--out dir]` | What an edit costs as the session grows (100, 1,000 and 5,000 clips on 50 tracks) while playing through the simulated sound card. Prints a Markdown table. |
| `engine_projection serve [--simulated-audio]` | The engine as a process the TypeScript side drives (contract/README.md). |
| `npm run dropout-check -- --bin <binary>` (in `ts`) | The dropout check on a real device: see below. |

Options for all three engine commands: `--tracktion-wave-nodes` plays clips the way Tracktion does by default, which waits on its file cache on the audio thread (finding 4); `--audio-cpus N` limits how many CPUs Tracktion uses for audio (finding 5). With a real device (`serve` without `--simulated-audio`, `bench --device`), `--device-type NAME` opens that JUCE device type instead of the system's default, such as `"Windows Audio (Exclusive Mode)"`, which bypasses the Windows mixer; a name that does not exist gets the list of the ones that do. `--buffer-size N` asks the device for N samples per block.

### The dropout check

`npm run dropout-check` builds a session through the model (8 tracks, 48 clips), plays an 8-second loop on the default output device, and then:

1. **quiet**, 10 s with no edits: the baseline for this machine;
2. **edits**, 30 s of random model edits at 10 a second (moves, resizes, adds, removes, undos, track adds and removes), half of them at the playhead; halfway through it empties the engine and rebuilds it from the model while it plays, and says so;
3. **after**, 5 s with no edits.

For each phase it reports the device's dropout counter, the click detector's clicks and gaps on the probe, and transport stops nobody asked for (#101). At the end it checks the engine still matches the model, prints a verdict, and saves a JSON report in the current folder. Listen on headphones: the **left ear** is the steady tone and must stay clean through everything; the **right ear** is the clips, jumping about as they are edited.

It also records the engine's output as it goes to the device and measures it in 50 ms windows: the left tone's level and any dips in it, anything on the left that is not the tone (such as the right side leaking across), and samples over full scale. Something you hear that is not in that recording happens after the engine, in the operating system's mixer, the interface or the headphones. `--record <file.wav>` saves the recording to listen to or inspect.

Its options: `--seconds`, `--rate`, `--tracks`, `--clips`, `--samples <folder>` (use your own .wav and .aif files instead of generated tones), `--simulated-audio`, `--tracktion-wave-nodes`, `--audio-cpus`, `--device-type`, `--buffer-size`, `--record`, `--seed`. They are listed at the top of `ts/src/dropout-check.ts`.

## Findings

Measured on a 2-core cloud machine (Intel Xeon at 2.1 GHz, Linux, no real-time scheduling) with the simulated sound card at 44.1 kHz and 441 samples, unless a finding says otherwise. Findings 4, 5, 6 and 10 end with what Chad's Windows machine showed, and finding 12 sums it up. Finding 13 is the Mac.

1. **The projection works for tracks and audio clips.** Edits in the model change what Tracktion plays: in self-test B, half a second before the playhead got there, a clip was moved to another track and later time, another removed, and a third added on a new track. The recording by edit time had the moved clip only at its new place (-6 dB where it should be, below -120 dB where it was), nothing where the removed one was, and the added one where it was put. In the end-to-end tests, 25 random sequences of 40 model edits each (adds, moves, resizes, removes, undos, track adds and removes) matched the projection exactly after every batch, read back from Tracktion's own objects. 300 longer sequences against the reference semantics did the same. Applying the diff of 2,000 random pairs of documents always gave the target.

2. **The projection is one-way, and the engine can be rebuilt from the model at any time.** Nothing is read from the engine into the model. A readback is used only to find out what to send. A second engine process that had never seen the session, given the projection of a 150-edit model, read back exactly what the first one held. Emptying the engine and resending everything while playing (self-test D, 60 clips on 12 tracks) took 6 to 12 ms to apply and 1 to 5 ms to rebuild the graph, with the probe clean. A readback costs 0.5 ms at 100 clips, 3.4 ms at 1,000 and 13 ms at 5,000, cheap enough to verify the engine every few seconds. A command lost without an error is invisible to the diff, which compares against what was sent, so only `verify` finds it; the product should verify on a timer and after every reconnect.

3. **Write clip positions in absolute terms; Tracktion's helpers have a trap.** `Clip::setLength (length, preserveSync = false)` moves the clip's offset into its file, so a shortened clip plays a different part of the file. `setStart` takes the same flag with the opposite meaning. Contract example 02 caught it: a clip shortened from 0.5 to 0.25 s came back with an offset of 0.25. The engine now builds the whole `ClipPosition` and calls `setPosition`. Same family as engine-toolchain finding 5: Tracktion's convenience setters do more than their names say.

4. **Tracktion's default clip node waits on the audio thread, and every edit sets it waiting. This is what caused dropouts.** Tracktion plays a clip that may use a proxy file (every clip, by default) with `WaveNode`. Its reads from Tracktion's file cache wait up to 3 ms each, on the audio thread (`isOfflineRender ? 5000 : 3` in `WaveNode::processSection`), spinning on `sched_yield` while the cache's reader lock is busy. Every edit rebuilds the whole playback graph (finding 6), and the new graph makes a new cache reader for every clip, so the cache's own threads remap and take that lock. Samples of the audio thread's stack during edits found it in `AudioFileCache::CachedFile::read`, yielding. With `WaveNode`, the simulated sound card ran late during edits and never without them:

   | | No edits | Edit storm (self-test C, 60 clips) | One move at 1,000 clips | One move at 5,000 clips |
   | --- | --- | --- | --- | --- |
   | `WaveNode` (Tracktion's default) | 0 late, slowest 2.4 ms | 90 to 103 late, slowest 56 to 76 ms | 5 to 8 late, slowest 120 to 131 ms | 7 to 14 late, slowest 92 to 137 ms |
   | `WaveNodeRealTime` (proxies off) | 0 late, slowest 1 to 3 ms | 0 late, slowest 2 to 3 ms | 0 to 1 late, slowest 17 ms | 0 late, slowest 1.5 ms |

   Turning proxies off on a clip (`setUsesProxy (false)`) makes Tracktion play it with `WaveNodeRealTime`, whose cache reads have a time-out of 0 ms: on a cache miss that clip is silent for the block, instead of the whole output being late. The spike now does that by default (`--tracktion-wave-nodes` brings the old behaviour back for comparison). Clips edited half a second ahead still played in full (self-test B passes either way). Not tested: compressed files and stretched clips without proxies, where `WaveNodeRealTime` decodes or stretches in real time at a CPU cost (DAW-8). Either way, NFR-3 says the audio thread never waits on the disk, and Tracktion's default does, indirectly.

   On Chad's Windows machine (16 cores), `--tracktion-wave-nodes` made no difference: no device dropouts with either node type, through 287 edits and a rebuild while playing. The waiting is still there; a machine with cores to spare and small, cached sample files hides it. Keep `WaveNodeRealTime`: the rule is that the audio thread never waits, not that it waits briefly enough.

5. **On a small machine, Tracktion's worker threads can make the audio thread wait too.** Tracktion processes the graph on a pool of threads, one fewer than the CPUs, and the audio callback waits for them. On this 2-core machine, a graph rebuild on the message thread can take the CPU the worker needs. The slowest blocks then took 92 to 137 ms of wall time but under 1.2 ms of the audio thread's own CPU: it was waiting, not working. With `WaveNodeRealTime` this drops to one late block of 17 ms at 1,000 clips; with `--audio-cpus 1` (no pool) there were none at any size. A desktop with 8 or more cores and a real-time audio thread may never see it. It needs measuring on Chad's machines before deciding anything (#27 owns device and thread settings).

   On Chad's Windows machine (AMD Ryzen 9 9950X3D, 16 cores, 32 threads), Tracktion's default of 32 audio threads was fine: the simulated sound card's slowest block was 0.42 ms against a 10 ms budget at 5,000 clips, none late (the audio thread's own CPU time is not measured on Windows). On the real device, dropouts with no edits at all came and went with other audio apps: with Spotify open, the default setting counted 1 to 13 dropouts in 15 s of quiet; closed, none. `--audio-cpus 4` was clean even with Spotify open, and `--audio-cpus 1` was the worst, but two things changed between those runs, so this does not settle a thread count. #27 should measure it with other audio apps running, because users will have them.

6. **Every edit rebuilds Tracktion's whole playback graph on the message thread, and that grows with the session.** A one-clip move costs well under a millisecond to apply, then Tracktion rebuilds the graph for the whole edit:

   | Clips (50 tracks) | Add all in one batch, then its rebuild | One move: apply | One move: graph rebuild | Readback |
   | --- | --- | --- | --- | --- |
   | 100 | 15 ms, then 4 ms | 0.09 ms | 4.3 ms (max 7) | 0.5 ms |
   | 1,000 | 107 ms, then 17 ms | 0.20 ms | 12.5 ms (max 18) | 3.4 ms |
   | 5,000 | 454 ms, then 61 ms | 0.52 ms | 63 ms (max 96) | 13 ms |

   JUCE's message thread is also where the web view bridge runs, so at a few thousand clips each edit would cost the interface several frames. The rebuild is per batch, not per command, so the product should send at most one batch per display frame and coalesce everything in between. The projector does that naturally: the diff of many model changes is one batch. Rebuilding only the changed part of the graph would need changes in Tracktion itself.

   On Chad's Windows machine the same bench was about twice as fast, which still makes 5,000 clips cost about two frames at 60 Hz per edit:

   | Clips (50 tracks) | Add all in one batch, then its rebuild | One move: apply | One move: graph rebuild | Readback |
   | --- | --- | --- | --- | --- |
   | 100 | 6.4 ms, then 1.6 ms | 0.05 ms | 1.9 ms (max 2.1) | 0.2 ms |
   | 1,000 | 43 ms, then 7.9 ms | 0.07 ms | 5.8 ms (max 8.2) | 1.5 ms |
   | 5,000 | 211 ms, then 37 ms | 0.20 ms | 29 ms (max 39) | 8.1 ms |

   That is `bench --device`, playing through the Focusrite. With the simulated sound card the rebuilds were similar (2.0, 7.9 and 32 ms per move).

7. **Graph swaps do not glitch audio the edits did not touch.** In every run, with either node type, the probe stayed clean: no clicks, no gaps, largest step 0.0016 against a threshold of 0.02. Every late block above was lateness, not a corrupted signal. On a real device lateness is the dropout, which is why finding 4 matters.

8. **Edits cut clips that are sounding.** Moving, shortening or removing a clip while it plays stops it at once. The detector counted about 700 hard edges on the clips' side in 8 s of storm. That is not a dropout, but the other person's edit will click in your ears. The product should give an edit to a sounding clip a short fade, which DAW-2 needs anyway.

9. **The model has gaps the projection exposed.** There is no clip offset, so a trim from the left cannot be expressed (DAW-2): the projection sends `offset: 0` and resizes from the end. There is no tempo, so ticks become seconds at a fixed 1,920 a second (960 per beat at 120 BPM); a tempo change will move every clip in one batch. A clip whose sample is not on this machine is left out and listed as missing; the product will need a placeholder while it transfers (#16). Ids live on Tracktion's items as a `collabgodId` property, with a map from id to Tracktion's `EditItemID` for lookups.

10. **The start-up and MIDI problems in #101 did not affect any of this.** The engine waits until it has settled before playing and restarts a transport that stops by itself, as the web view spike does, and counts those restarts separately. None happened in any run here, which used the simulated sound card. On real machines plugging a MIDI controller in during the dropout check will show up as an unexpected stop, not as a projection failure.

   On Chad's Windows machine it did: plugging a MIDI controller in during the edits stopped the transport, and the engine counted it and pressed play again. The restart is not seamless. The click detector counted one click on the probe, and the recording shows the probe 1.8 dB down in one 50 ms window around it. That is #101's problem, not the projection's, but it is audible, so whatever #101 settles on should not stop playback in the first place. The check used to call that run a FAIL; it now calls a stop during the edits with no device dropouts INCONCLUSIVE and asks for another run.

11. **The dropout check's first sessions overloaded the output, and that confused the listening.** Tracktion adds 6 dB to a track panned hard right, and with 48 clips on 8 tracks up to about nine tones overlap, so the right channel peaked at +14.4 dBFS with the generated tones and +17.7 dBFS with a folder of real samples. The left channel was clean throughout (the recording shows the right side at least 62 dB down on the left, and no dips), but on Windows a dip in the left tone and some right-side sound in the left ear were heard. The dip did not come back once other audio apps were closed; the bleed was still heard in exclusive mode, which bypasses the Windows mixer, so it is after the driver or in the hardware. Every model track now has -24 dB of gain (`Host::modelTrackGainDb`), and the check measures its own output and warns when it goes over full scale. The product will need a real mix bus with headroom; DAW-level gain staging is not this spike's.

12. **On Chad's Windows machine, edits during playback did not cause a dropout.** Windows Audio (shared mode) through a Focusrite USB interface, 44.1 kHz, 441 samples per block, all 32 threads for audio, other audio apps closed. With generated tones, with `--tracktion-wave-nodes`, and with a folder of Chad's own samples: 287 edits in 30 s plus a rebuild from scratch while playing, and in every phase 0 device dropouts, 0 probe clicks or gaps, the left tone never more than 1 dB down, and the engine matching the model at the end. Listening on headphones agreed. Model to engine took a median of 3.7 to 4.0 ms per edit, round trip through the engine process, and p95 about 6 ms. Exclusive mode, tried while looking into finding 11, dropped out constantly at its default block size; that is a device setting for #27, not a projection result.

13. **On Chad's Mac, edits during playback caused dropouts with Tracktion's default thread pool, and one audio thread cleared them.** Intel MacBook Pro, 4 cores, 8 GB, macOS 13, built-in output through CoreAudio, 44.1 kHz, 512 samples per block, `WaveNodeRealTime`, other apps open (so this is a small, busy machine, and the counts are small). The click detector and the recording of the output stayed clean in every run, and they could not have shown this: they measure what the engine generates, and a late callback still generates a clean block. The first runs printed PASS and Chad heard a large dropout towards the end of the edits phase. The check only counted dropouts where the device has its own counter, which CoreAudio does not. `Host::getXRuns` now uses the device manager's count, which adds the callbacks JUCE times as longer than their block, and the check says FAIL. Late callbacks during the edits phase (the quiet and after phases were 0 in every run):

   | Audio CPUs | Late callbacks, one run each | Slowest graph rebuild | Callback load (smoothed) |
   | --- | --- | --- | --- |
   | 4 (default) | 6, 1, 5, 3, 4 | 1,021 to 1,055 ms | up to 90 to 100% |
   | 2 | 3, 5, 6, 5 | 1,024 to 1,033 ms | up to 100% |
   | 1 | 0, 0, 0, 0 | 4.7 to 11.9 ms | 5% |
   | Heavier session (16 tracks, 200 clips), 4 | 3, 4 | about 1,025 ms | up to 90% |
   | Heavier session, 1 | 0, 0 | 7.9 to 8.4 ms | 11% |

   Six of six runs with one audio CPU had no late callback, and every run with more than one had at least one. Two CPUs was no better than four, so this is one thread against several, not a gradient (an earlier version of this finding said otherwise, from fewer runs). The median graph rebuild was the same in every run (2.7 to 2.8 ms, 4.6 to 5.7 ms in the heavier session); what differs is a single rebuild of about 1.02 to 1.05 s, in every run with a thread pool and none without. A fixed second is not what scheduling noise looks like, so something waits with a time-out of about that length when worker threads exist. **Not found:** a `sample` of the engine during an edit storm showed the audio thread inside `LockFreeMultiThreadedNodePlayer::process` and `ThreadPoolSemHybrid::waitForFinalNode` for a few dozen samples, but no frame holding anything near a second, so the sample did not catch the stall, and the cause is unproven. The simple explanation, that the audio thread waits for worker threads that the operating system has not run because a 4-core machine has nothing to spare, fits the data and Windows's 32 free threads, but has not been shown. What it does not say: that one thread is enough. These sessions are generated tones with no plugins, and a single audio thread only used 5 to 11% of its block here. #27 owns the thread setting and should test it with plugin load and other audio apps running.

## Cost and recommendation

**Keep the TypeScript model as the source of truth, with the engine as a projection.** Do not switch to the fallback in ADR-0002 (Tracktion's edit as the source of truth).

The projection itself was cheap. It is about 510 lines of TypeScript (`document`, `diff`, `commands`, `projector`) and 380 of C++ (`Commands`), for tracks and audio clips. Each new kind of thing (MIDI clips, automation, plugin parameters) adds a command family on both sides and its contract examples. The diff and the reference semantics are pure and property-tested without the engine, and a readback makes drift detectable in milliseconds. The hard parts were all inside Tracktion, and they would be there under the fallback too:

- clips must be played without proxies (`WaveNodeRealTime`), or edits during playback cause dropouts (finding 4);
- every edit rebuilds the whole graph on the message thread, so edits must be batched per frame, and large sessions will need watching (finding 6);
- Tracktion's convenience setters cannot be trusted to do only what they say, so the engine side writes absolute values (finding 3).

None of these is made worse by the projection. The fallback would put the same edits through the same graph rebuilds, and would lose the property tests and the browser-safe model.

On Windows, neither happened (findings 4, 6 and 12): no dropouts, and a rebuild of 29 ms per edit at 5,000 clips, which batching per frame covers. What would change the recommendation: if the Mac drops out during the check even with `WaveNodeRealTime`, or if the graph rebuild on a real session is slow enough to hurt the interface even when batched. Either would point at Tracktion, not at the projection, and would be a reason to look at the fallback engine choices in ADR-0002 rather than at the source-of-truth choice.

On the Mac (finding 13) the condition above was met in part: with other apps open on a 4-core machine, edits during playback caused late audio callbacks with `WaveNodeRealTime` and Tracktion's default thread pool, and none with one audio thread. The recommendation stands, because it points at Tracktion's threading and not at the projection or the fallback, and it comes with a setting that cleared it. It adds a requirement: the app needs a thread count it can set and a count of late callbacks in its own diagnostics, because on a Mac the click detector and the device cannot report them. The unexplained one-second graph rebuild that goes with the thread pool needs a cause before the default is trusted on any machine.

## What is left (needs Chad's machines)

The time box is spent on what a cloud machine can show. These need the real devices, and the issue stays open until they are done:

- [x] Windows: build, `ctest`, and `npm test` with `ENGINE_PROJECTION_BIN` set.
- [x] Windows: `npm run dropout-check`, listening on headphones. Then again with `--tracktion-wave-nodes`, to hear and count the difference. Then with `--samples` pointing at a folder of real samples (finding 12).
- [x] Windows: `engine_projection bench`, for the cost table on a real CPU (finding 6).
- [x] Windows: plug a MIDI controller in during the edits phase once (finding 10).
- [x] Mac: `ctest` (passes), `npm test` with the binary (29 of 29), `bench` (finding 13's rebuild costs: 3.6 ms at 100 clips, 14 ms at 1,000, 69 ms at 5,000) and the dropout check with different thread counts (finding 13). Not done on the Mac: `--tracktion-wave-nodes`, `--samples`, and a MIDI controller during the edits.
- [ ] Find the cause of the one-second graph rebuild with a thread pool (finding 13).
- [ ] Post the dropout check's summary tables and the bench tables on #11, and attach the JSON reports.

## Not covered

- MIDI clips, automation, plugin parameters, mixer changes, track renames. The commands for them follow the same pattern; plugin parameters are #12.
- The web view bridge in the loop. #8 measured it at 0.1 to 1 ms a round trip, against about 4 ms here for stdio and a forced graph rebuild.
- Compressed and stretched clips without proxies (finding 4), clip fades on edit (finding 8), tempo changes (finding 9).
- Two people. The projection takes whatever the model's `view` is; who made the edit does not matter to it. The relay is #10.
