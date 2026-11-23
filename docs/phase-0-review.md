# Phase 0 review

- **Status:** Six of the seven [decisions](#decisions) are made. The go or no-go is not recorded.
- **Date:** 2026-11-22
- **Issue:** [#17](https://github.com/chadalon/collabgod/issues/17)

Phase 0 asked whether the three novel pieces work between two real machines, and whether the proposed stack holds. This page collects what the spikes found, checks it against the gate and the numeric targets, and lists what Phase 1 inherits. The detail stays in each spike's README and in the decision records; this page links to them.

## The gate

The roadmap's gate: plugin sync, sample matching and transfer, and edit convergence all work between two real machines in different cities, and the stack record is accepted or replaced.

| Piece | Between two real machines | Between two cities | Verdict |
| --- | --- | --- | --- |
| Samples: index, match, transfer | Yes | Yes. Chapel Hill and Park City, 2026-11-21: both real libraries indexed and compared, 190.7 MiB sent each way, one transfer resumed after a cut | Met |
| Edits: one order through a relay | Yes | Partly. Through a cloud relay between Chapel Hill and Park City, 200 of 200 operations were ordered and answered and a late joiner caught up. Two replicas editing at once converged over real sockets on one machine, not between the two cities | Met for ordering and delay. Convergence between two cities not run |
| Plugins: parameters and state | Yes. A Windows PC and an Intel Mac with two people, Pro-Q 2, Pro-Q 3 and Pro-L 2. Serum 2 on one machine only, two copies | No. Both machines were on one network in Chapel Hill | Met between two machines. Not run between two cities |

The plugin spike was not run between the two cities because its app needs macOS 12 and the Park City machine is on macOS 11.7 (#145). The network leg it would add is the one the relay and sample runs already measured. Chad decided on 2026-11-22 that this meets the gate (decision 1): between two cities plugin changes will arrive later than they did on one network, and the product has to be built for that delay.

## What each spike found

| Issue | Question | Answer | Written up in |
| --- | --- | --- | --- |
| #7 | Do JUCE and Tracktion Engine build on both systems and in CI? | Yes, on JUCE 9 with Tracktion's `develop` pinned to a commit. Apple clang 15 needs two small patches. A whole CI job from cold takes under 5 minutes on Windows and about 8 on macOS | [`engine-toolchain`](../spikes/engine-toolchain/README.md), ADR-0002, ADR-0009 |
| #8 | Does a TypeScript interface in the web view hold 60 fps while the engine streams to it? | Yes, on both of Chad's machines, with 10,000 clips and no frame over 20 ms. WebGL2 is the floor; WebGPU is not available on the Intel Mac | [`webview-ui`](../spikes/webview-ui/README.md), ADR-0002 |
| #9 | Does a relay-ordered log with optimistic local apply always converge? | Yes, with two and three clients under random delivery. Undo must be guarded by revisions, not values. A whole-state copy per operation is too slow for the product | [`session-model`](../spikes/session-model/README.md), ADR-0004 |
| #10 | Can two homes reach a relay over TLS, and how long does an edit take? | Yes. Chapel Hill to Park City: 49 ms at the median, 54 ms at the 95th percentile, one run | [`relay`](../spikes/relay/README.md), ADR-0004 |
| #11 | Can the TypeScript model drive Tracktion Engine without the two drifting? | Yes for tracks and audio clips. Keep the projection; the all-C++ fallback would have the same hard parts. Clean on Windows. On the 4-core Mac, edits during playback gave late audio callbacks unless the engine used one audio thread | [`engine-projection`](../spikes/engine-projection/README.md), ADR-0002 |
| #12 | Do VST3 parameter changes mirror between two instances? | Yes, with Serum 2, Lounge Lizard Session 4 and Pro-Q 2, including two machines and two plugin versions. Parameters must be matched by VST3 parameter ID, never by index | [`plugin-params`](../spikes/plugin-params/README.md), ADR-0002 |
| #13 | Can state that is not a parameter be synced? | Yes: presets, LFO shapes, modulation and wavetables follow within about a second. Serum 2 goes silent for about 4 s on the copy that receives a state. Two presets clicked at the same moment can end as a mix of both | [`README-STATE.md`](../spikes/plugin-params/README-STATE.md), ADR-0011 |
| #14 | What counts as the same plugin on two machines? | The VST3 class ID in its canonical text form. All 9 plugins shared by Chad's two machines differed in version, so a version difference is flagged, not blocked | [`plugin-inventory`](../spikes/plugin-inventory/README.md), ADR-0010 |
| #15 | Does the same audio get the same identity in both libraries? | Yes. 70.8% of the smaller real library resolves from the larger one with no transfer. The audio hash matched 1,001 files the file hash missed, so it stays | [`sample-index`](../spikes/sample-index/README.md), ADR-0008 |
| #16 | Does a missing sample arrive by itself, resumably, without holding up edits? | Yes. 20 to 38 Mbit/s between the two homes, resumed after a cut, every file verified. Sending from the slower uplink delayed edits more than intended | [`sample-transfer`](../spikes/sample-transfer/README.md) |

## Measurements against the numeric targets

`docs/requirements.md` called its numeric targets proposals to confirm in Phase 0. Chad made them final on 2026-11-22 as the last column says (decision 6), and requirements v0.10 carries the changes.

| Target | Measured | Decided |
| --- | --- | --- |
| NFR-1: an edit reaches the other person within 150 ms | 49 ms median, 54 ms at the 95th percentile between the two homes when idle. While the slower uplink was sending a sample, its ping rose by 20 ms at the median and 91 ms at the 95th percentile, so the 95th percentile still fits with little margin and the worst cases do not (#140) | Keep 150 ms. It holds during a sample transfer too, so #140 is measured against it |
| NFR-2: an idle lobby uses less than 10 kbps | Not measured in Phase 0. A moving plugin control costs about 2 kB a second at 30 messages a second | Stays a proposal until Phase 2 measures it |
| NFR-4: a remote edit never causes a dropout, and another member's preset load sounds no worse to you than to them | Clip edits: no dropouts on Windows; on the 4-core Mac only with one audio thread and clips played without proxies. Preset load: not met with Serum 2, about 4 s of silence on the receiver | Keep. Record the Serum 2 gap as a known Phase 3 risk with its untried remedy (load into a second instance, then switch) |
| NFR-5: indexing 100,000 files never blocks the app, and later scans read only changed files | Largest real library: 32,460 files, 88 GB, indexed in 89 s on a fast drive; 12,112 files in 258 s on a hard disk. Event loop delay 14 ms at worst. A rescan with nothing changed read 0 bytes in 1.3 s | Keep. The 100,000-file figure was reached only with generated files |
| UX-2: the timeline, meters and waveforms hold 60 fps | 60 fps in every benchmark phase on both machines; a command to the engine was heard 16 ms later at the median | Keep. Add that drawing uses WebGL2, since WebGPU is missing on macOS before 26 |
| Sample transfer added delay (no requirement yet) | The delay-based window aims for 25 ms of added delay; 24 to 41 ms measured through a throttled link | 25 ms is accepted, as long as the other person's edits still arrive smoothly (decision 5). Now part of NFR-1 |

## Decision records

| Record | Before | Now | Why |
| --- | --- | --- | --- |
| ADR-0002, stack | Proposed | Accepted | All three of its "How we will know" questions came back yes: the web view holds 60 fps (#8), the model drives the engine without drift (#11), and parameter and state sync work with Serum 2 and others (#12, #13). Its section on #13 was brought up to date with the real-plugin results in the same change |
| ADR-0004, ordered edit log | Proposed | Accepted | No divergence in the randomized tests, and the relay ordered every operation between two cities. Two things it listed as still to confirm move to Phase 2: the client's half of resending operations after a reconnect, and convergence between two locations (decision 2) |
| ADR-0009, C++ dependencies | Proposed | Accepted | Its test was that the pinned fetch works on GitHub's Windows and macOS runners and on the maintainers' machines. It did, on the runners and on Chad's Windows desktop and Intel Mac. Not run on the Park City iMac (#145) |
| ADR-0003, 0005 to 0008, 0010 to 0012 | Accepted | No change | |

The issue's checklist names ADRs 0002 to 0004. ADR-0003 was accepted on 2026-11-11.

## What Phase 1 and later inherit

Found in Phase 0 and not solved there. Each needs an owner issue before its phase starts.

| Finding | Where it lands | Tracked |
| --- | --- | --- |
| A change in the MIDI device list stops the transport, and resuming clicks. HW-1 is a Must | Phase 1 | #101, #125 |
| On a small Mac the engine's thread pool gives late audio callbacks during edits, with an unexplained one-second graph rebuild. The app needs a settable audio thread count and its own late-callback count | Phase 1 | #113, #27 |
| Clips must play without proxies (`WaveNodeRealTime`), and edits reach the engine at most once per display frame in one batch | Phase 1 engine projection | `engine-projection` README, "What the product must do" |
| The session model needs structural sharing or rollback by inverses: 41 ms to take one remote edit on a 5,000-clip session with a whole-state copy | Phase 1 session model | ADR-0004 |
| Live parameter values need smoothing on the audio thread or they click | Phase 1 engine, Phase 3 plugins | `engine-toolchain` finding 9 |
| A returning member should take over their old connection's place; the heartbeat cannot tell a stalled connection from a dead one | Phase 2 relay | #36, #48 |
| The client's half of resending operations after a reconnect, and convergence between two locations | Phase 2 sync client and lobby tests | ADR-0004 |
| Sending a sample from a slow uplink delays edits by more than the window aims for | Phase 2 | #140 |
| FLAC streams saved with a `.wav` extension get no audio identity | Phase 2 | #141 |
| Plugin changes between two cities arrive about 50 ms later than on one network, and plugin sync has not been run over that path | Phase 3, at the first two-city plugin session | Decision 1 |
| Serum 2 is silent for about 4 s after loading a received state | Phase 3 | ADR-0011, NFR-4 |
| A preset change must travel as one unit, and a copy must not send back a state it loaded | Phase 3 | ADR-0011 |
| A plugin state can name a file the other machine lacks | Phase 3 | #117 |
| The Park City machine runs macOS 11.7 and the engine builds need macOS 12. The Phase 1 gate is checked on that machine | Phase 1 | #145 |

## The competitor session (#4)

Issue #4 asks for one session each in StarSync Studio and Audiotool with both producers, written up in `docs/research/competitors.md`. It is the only Phase 0 item that is not a spike or a decision about our own design.

What it is for:

- **Whether the two differentiators are really different.** Nobody has confirmed what either product does when one person lacks a plugin, or whether either matches samples against each person's own library.
- **How they handle what Phase 0 found hard.** State that is not a parameter, two people on one plugin (StarSync lets one person edit a plugin at a time), and how late the other person's edits arrive.
- **What felt good and what was missing**, which only two producers working on a real idea can say.

What it cannot change: the stack, the edit log, sample identity or plugin identity. Those were decided from our own measurements. Its findings would change requirement priorities and interface design in Phases 2 and 3, and Phase 1 builds neither.

Chad decided on 2026-11-22 not to hold the review for it (decision 3). #4 moved to the Phase 1 milestone, to be done before Phase 2 planning starts. Until then the two differentiators are unconfirmed.

## Decisions

Made by Chad on 2026-11-22:

1. **The gate is met.** Plugin sync between two machines on one network counts. The delay between two cities is expected and the product is built for it.
2. **ADR-0004 is accepted.** The client's resend handling and convergence between two locations move to Phase 2.
3. **Issue #4 moves to Phase 1**, due before Phase 2 planning starts.
4. **ADR-0002 and ADR-0009 are accepted.**

5. **25 ms of added delay during a sample transfer is acceptable**, on one condition: it must not be tiring or frustrating for the person receiving the edits. Things still have to feel smooth on their end. The slower-uplink case that overshot the target (#140) is judged against that.
6. **The numeric targets are final** as the table above says. NFR-2 stays a proposal until it is measured.

Open:

7. **Go or no-go** for Phase 1.

## Still to do before #17 closes

- [x] ADR-0002's section on #13 brought up to date, and ADR-0002, ADR-0004 and ADR-0009 moved to Accepted
- [x] #4 moved to Phase 1
- [x] `docs/requirements.md` revised from the table above (v0.10)
- [x] #103 closed and the relay taken down
- [ ] Phase 1 issues re-estimated against the inherited findings, and the first sprint planned
- [ ] `docs/roadmap.md` updated: Phase 0 closed, risks table brought up to date
- [ ] Go or no-go recorded on #17, which closes the epic #1
