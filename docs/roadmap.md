# Roadmap

Six phases. Each ends in something two people can run, and each has a gate that decides whether the next one starts. Due dates are kept on the [milestones](../../../milestones), not here. Later phases get theirs at sprint planning, from weekly capacity.

**Now: Phase 1.** Phase 0 closed on 2026-11-23 with a go ([review](phase-0-review.md)).

## Phase 0: decide and spike (closed 2026-11-23)

Settle the open decisions, then build throwaway proofs of the three novel pieces.

- Two app instances host the same VST3. A knob turned in one moves in the other, and a preset change carries over.
- Two sample libraries are indexed, a shared file matches by content, and a missing file transfers with progress.
- Two clients edit a toy clip list through a relay and always end in the same state.

**Gate:** all three work between two real machines in different cities, and the stack ADR is accepted or replaced.

**Result:** met, with the stack accepted (ADR-0002). Samples and edit ordering ran between Chapel Hill and Park City. Plugin sync ran between two machines on one network, which the maintainer accepted as meeting the gate. What later phases inherit is listed in the [review](phase-0-review.md).

## Phase 1: walking skeleton

App shell, audio output, one timeline, audio clips, play and stop, save and load. CI green on Windows and macOS. The design system starts here. Plugging in a MIDI device never interrupts playback (HW-1).

**Gate:** place a sample, hear it, save, reopen.

## Phase 2: the lobby

Invite codes, presence, live clip edits, and sample matching and transfer inside the timeline.

**Gate:** two people arrange audio clips together in one session.

## Phase 3: instruments

VST3 hosting, MIDI clips, piano roll, plugin inventory comparison, parameter and state sync.

If time allows: MIDI keyboard input and recording, controllers that play the armed track with no setup (HW-2), and hearing and seeing each other's live playing (LIV-1 to LIV-3).

**Gate:** two people tweak one synth patch and edit its notes live.

## Phase 4: finish a track

Automation, mixer, per-person undo across every edit type, export.

If time allows: MIDI learn for any control (HW-3).

**Gate:** the v1 test. One full track, no files sent by hand.

## Phase 5: identity and alpha

Visual effects, polish, first-run experience, installers, a promotional website, and a small group of outside testers.

**Gate:** outside testers complete a session without help.

## Hard problems to keep in view

| Risk | First move |
| --- | --- |
| Scope: a full DAW is years of work | Spikes first, then a thin DAW built around the lobby. Cut features before cutting quality. |
| Plugin state that is not a parameter | Sync parameters as events; send the state blob when a non-parameter change is detected. |
| Plugin drift between two installs | Detect version mismatches. Accept small audible differences and say so in the app. |
| Concurrent edits and undo | One ordered edit log through the relay, per-person undo, randomized convergence tests from day one. |
| Sample identity | Exact content hash for automatic matches. Anything fuzzy is a suggestion the user confirms. |
| Sharing licensed samples | Licensing review before any public release. |
| Connectivity | Start with a small relay. Add direct peer connections later for file transfer. |
| Real-time safety | Lock-free queues between threads, enforced by review and tests. |
| The engine stops playback when the MIDI device list changes | Found in Phase 0 (#101). Pressing play again is not enough: it clicks. Fix it in Phase 1 (#125). |
| Loading a plugin's state can interrupt its sound (Serum 2: about 4 seconds) | Found in Phase 0 (#13). Load as rarely as possible: send parameter values exactly, and skip the load when both copies already save the same state. Untried remedy: load into a second instance in the background, then switch over (ADR-0011). |
| On a small Mac the engine's thread pool gives late audio during edits | Found in Phase 0 (#11). One audio thread cleared it. Find the cause and choose a safe default in Phase 1 (#113, #27). |
| The second maintainer's iMac is on macOS 11 and the app needs 12 | Found in Phase 0. Settle it early in Phase 1 (#145): the Phase 1 gate is checked on that machine. |
| Plugin sync has not run between two cities, through the TypeScript model or through the web view bridge | Phase 0 synced app instances on one network. First two-city plugin session in Phase 3 (ADR-0002). |
| The differentiators are unconfirmed against StarSync and Audiotool | Competitor session (#4), before Phase 2 planning. |
