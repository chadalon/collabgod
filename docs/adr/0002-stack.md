# ADR-0002: C++ engine, TypeScript interface and session model

- **Status:** Accepted
- **Date:** 2026-11-11 (accepted 2026-11-22)
- **Deciders:** Chad

## Context

The product needs four things that pull in different directions:

1. Reliable hosting of third-party VST3 plugins and a low-latency audio graph on Windows and macOS.
2. A session model in which every change is an ordered operation, so two machines converge.
3. A distinctive, GPU-drawn interface that can change quickly.
4. A small team whose strongest language is TypeScript, followed by Rust and C++.

The VST3 SDK, JUCE and Tracktion Engine are C++. Plugin hosting is where years of edge cases live, and it adds nothing to what makes this product different. Rust hosting crates exist but are at 0.x versions.

The parts that are new here, ordering, undo, convergence and presence, are pure logic with no audio dependency.

## Decision

- **Audio engine, plugin hosting, file and device access:** C++20 on JUCE 9 and Tracktion Engine. Current Tracktion Engine is built on JUCE 9; its last JUCE 8 release is from May 2025 (issue #7, decided 2026-11-13).
- **Interface:** TypeScript, running in JUCE's web view, with the timeline, meters and effects drawn on a GPU canvas.
- **Session model, sync client and relay server:** TypeScript, in one package shared by the app, the relay and the tests.
- **Source of truth:** the TypeScript session model. The C++ engine holds a projection of it and is told what changed.
- **Rust:** not used in v1.

## Alternatives considered

- **Fork Ardour, LMMS or Zrythm.** None was designed around a shared, ordered stream of edits, and each brings an interface toolkit we would discard.
- **All C++, with the engine's project model as the source of truth.** One language and no projection layer, but the novel logic lands in the team's weakest language, is harder to property-test, and cannot be reused by a browser client. This is the fallback if the projection proves too costly.
- **Rust core with a C++ shim for plugins.** Three languages for a team of one or two.
- **From scratch on the VST3 SDK.** Full ownership, and all of the plugin-hosting edge cases.
- **Electron or Tauri shell with a native audio sidecar.** Two processes to ship and keep in step, and plugin windows still need a native host.

## Consequences

- Two languages and a typed bridge between them. The bridge contract needs its own tests.
- The engine and the model can disagree. The projection must be one-way and testable.
- The web view differs between Windows and macOS. GPU canvas support must be checked on both.
- The session package can run headless in CI and, later, in a browser.
- The app must ship under a license compatible with GPLv3 (Tracktion Engine) and AGPLv3 (JUCE 9). See ADR-0003.

## How we will know

Phase 0 spikes answer these before the decision is accepted:

- Does the web view hold 60 frames a second while receiving playhead and meter data from C++?
- Can the TypeScript model drive Tracktion Engine for tracks, clips and plugin parameters without the two drifting?
- Does plugin parameter and state sync work through the bridge for Serum 2 and two or three other plugins?

## Findings

**Issue #8, web view at 60 fps** (`spikes/webview-ui`): **confirmed.** A TypeScript page in JUCE 9's web view draws 10,000 clips with WebGL2, and Tracktion Engine streams the playhead and stereo meters to it on every display refresh. On 2026-11-14 it held 60 fps in every phase of the benchmark on both maintainer machines, with no frame over 20 ms:
- Windows: WebView2 157, RTX 5080
- Intel MacBook: WKWebView on macOS 13.7.2

The phases included panning and zooming across all clips, 256 extra meter levels per frame, and re-uploading every clip to the GPU each frame. The page's own work per frame was under half a millisecond. A command to the engine was heard 16 ms later at the median on both, which is one display frame. The web view part of this ADR holds. Also learned:

- WebGL2 is the drawing floor. WKWebView gets WebGPU only from macOS 26, and the Intel Mac on macOS 13.7 cannot run it; WebView2 has it on Windows. The interface draws with WebGL2, and anything built on WebGPU needs a WebGL2 path too.
- WebKit is stricter than Chromium about shaders: it refused to link one that Chromium would accept. Every shader is tested in both web views.
- WebKit hides the GPU's name, so a report from a Mac cannot say which GPU drew it.
- The bridge is not the bottleneck: a native call came back in 0.1 ms on Windows and under 1 ms on the Mac at the median.
- Engine frames and drawn frames are not guaranteed to be in step (4% of drawn frames on the Mac had no new engine data, a quarter on a CI runner). The interface animates the playhead from positions and timestamps, not by counting events. Alternatively it asks C++ for data as it draws each frame, which kept every frame fresh everywhere.
- The canvas trails a window resize by a frame, briefly showing the web view's own background. The interface redraws as soon as the size changes, and the web view's background colour matches the page.
- The engine's start-up is more than the playback context: its own MIDI device scans can land after the context exists and stop the transport. The host waits for both to go quiet and resumes a transport that stops on its own (#101).
- On Windows the web view must be asked for explicitly: JUCE's default there is still Internet Explorer. The spike links the WebView2 loader statically, from the SDK downloaded at configure time.

**Issue #11, the TypeScript model drives Tracktion Engine** (`spikes/engine-projection`): **works for tracks and audio clips; keep the projection.** Verified on a cloud machine with a simulated sound card, and on a Windows desktop with a real interface: no dropouts from edits during playback, by counter, by click detector and by ear. On an Intel Mac with 4 cores it was not clean: see the Mac bullet below.
- A pure `project` turns the model's state into an engine document (seconds, local files), a pure `diff` turns two documents into ordered commands, and the C++ side only carries them out and reads back what Tracktion holds. Random model edits matched the projection exactly after every batch, read back from Tracktion. Clips moved, removed and added half a second ahead of the playhead were heard where they now were.
- One-way and rebuildable: nothing flows from the engine into the model. A fresh engine given the projection holds the same as one that saw every edit. Emptying the engine and rebuilding while playing kept the untouched audio clean. A readback costs 0.5 to 13 ms (100 to 5,000 clips), so drift can be checked on a timer; a silently lost command is only found that way.
- Edits during playback caused dropouts with Tracktion's default clip node, `WaveNode`. Its file cache reads wait up to 3 ms each on the audio thread, and every edit disturbs the cache by rebuilding the graph. Playing clips with proxies off (`WaveNodeRealTime`, which never waits) removed them: an edit storm went from about 100 late blocks (slowest 76 ms) to none. The app must play clips this way, or the same. On the 16-core Windows desktop neither node type dropped out; spare cores hide the wait rather than remove it.
- Every edit rebuilds Tracktion's whole playback graph on the message thread, where the web view bridge also runs: 4 ms at 100 clips, 13 ms at 1,000, about 60 ms at 5,000 for a one-clip move (about half that on the Windows desktop). Edits must be sent at most once per display frame, in one batch.
- On the Mac (4 cores, 8 GB, other apps open), edits during playback caused late audio callbacks with Tracktion's default thread pool: 1 to 6 per 30 s of edits with 4 or 2 audio threads, 0 in six runs with 1, including a heavier session (with Tracktion's default clip node, one thread still gave 2). The click detector cannot see this, and CoreAudio has no counter, so the check now uses JUCE's count of late callbacks. A graph rebuild of about a second goes with the thread pool and is unexplained. This does not change the recommendation, because it is inside Tracktion and the fallback would have it too, but the app needs a settable thread count and its own late-callback count (#27). Until the second-long rebuild has a cause, the thread pool is not a safe default on small machines; #113 investigates it, starting from the spike README's "Open question" section, and its "What the product must do" collects every requirement the spike found.
- Tracktion's `setStart` and `setLength` take a `preserveSync` flag with opposite meanings; the engine side writes whole positions.
- The projection was about 900 lines across both languages for tracks and audio clips. Every hard part was inside Tracktion and would be there under the all-C++ fallback too, so the fallback is not recommended.

**Issue #12, plugin parameter sync** (`spikes/plugin-params`): **works for parameters with a test VST3, Lounge Lizard Session 4, FabFilter Pro-Q 2 (between Windows and macOS) and Serum 2; plugin state does not sync through parameters.** Serum 2's LFO shapes, modulation assignments, wavetables and the loaded preset are not VST3 parameters and did not reach the other member (issue #13). Parameter changes from the plugin's own window and from the host mirror between members over TCP, move the other plugin and change its sound; echoes and two people on one control converge, and automation is played on each side instead of sent. What it means for this ADR:

- The sync rules are small and pure (last writer wins per parameter, gesture-aware, echo window, per-parameter throttle) and fit the TypeScript sync client. The C++ side must still swallow the callback caused by setting a remote value, read a plugin's final value when its gesture ends (Tracktion Engine reports values asynchronously and gestures at once, so the end can overtake the last value), and coalesce before the bridge.
- Tracktion Engine keys VST3 parameters by list index, which a plugin update can shift. The session and the wire must use the VST3 `ParamID`, which JUCE exposes; the class that reaches it, `ExternalAutomatableParameter`, is not in Tracktion's public headers.
- At 30 messages a second per moving control, about 2 kB a second, live changes belong outside the ordered log (relay `direct` messages), with only the value a gesture ends on recorded as an operation.
- Different versions of a plugin synced by ID (Pro-Q 2 2.2.2.0 against 2.1.1.0, all 190 parameters matched), so a version difference can be flagged rather than block sync.
- Presets travel as parameters only when the plugin announces them (Pro-Q 2 did, Lounge Lizard did not). Plugin state sync (#13) is needed regardless.

**Issue #13, plugin state sync** (`spikes/plugin-params`, [`README-STATE.md`](../../spikes/plugin-params/README-STATE.md)): **works with Serum 2, Sylenth1, FabFilter Pro-Q 3 and Pro-L 2, including between a Windows PC and a Mac with two people and two plugin versions; two gaps are left for the product.** A preset, an LFO shape, a modulation assignment and a wavetable each reached the other copy as the plugin's saved state within about a second. The gaps: Serum 2 is silent for about 4 s on the copy that receives a state, and two presets clicked at the same moment can end as a mix of both. Both are recorded in ADR-0011, and neither comes from the stack: any host has to load an opaque state the same way. What it means for this ADR:

- The rules are pure and share the parameter rules' clock (ADR-0011), so they fit the TypeScript sync client too. The C++ side reads and loads the state, hashes it, and watches for hints.
- VST3 state calls must run on the main thread, and a large state's read or load takes long enough to be seen there (120 to 150 ms for 10.7 MiB on a 2-core cloud machine). The engine host has to read rarely and show progress when loading a large state.
- JUCE's VST3 host wraps the state in XML with base64 (a third larger) and does not report whether the plugin accepted it. The product should keep the component and controller states as raw bytes and check a load by reading it back.

## Outcome

Accepted on 2026-11-22 at the Phase 0 review ([`docs/phase-0-review.md`](../phase-0-review.md), issue #17). The three questions under "How we will know" came back yes for what was tested, and two parts of them were not tested:

- **Yes:** the web view holds 60 frames a second with data from C++ (#8). The TypeScript model drives Tracktion Engine for tracks and audio clips without drift (#11). Parameter and state sync work for Serum 2 and others between app instances (#12, #13).
- **Not tested:** driving plugin parameters from the TypeScript model (#11 covered tracks and audio clips only), and plugin sync through the web view bridge (the #12 and #13 spike linked C++ app instances over TCP; its rules are pure and written to fit the TypeScript client). Both land in Phase 3. If either fails there, a new record supersedes this one.

What the spikes found that the product must do differently is listed in that review and in each spike's README.
