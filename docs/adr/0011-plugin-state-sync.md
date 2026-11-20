# ADR-0011: Plugin state syncs as a content-addressed snapshot, announced by hash

- **Status:** Proposed
- **Date:** 2026-11-15
- **Deciders:** Chad

## Context

Parameter sync (#12) gives two people the same knob values, but many plugins keep part of what makes their sound outside their parameters: Serum 2's LFO shapes, modulation and wavetables, the loaded preset, embedded or referenced samples. Some plugins also change parameters on a preset load without telling the host. PLG-6 asks for this part to sync "by sending the plugin's saved state".

A host cannot see inside a plugin's saved state. It can only ask the plugin for all of it (VST3 `IComponent::getState` and `IEditController::getState`, which JUCE wraps as `getStateInformation`) and give all of it to another copy. It cannot tell what changed, merge two changes, or remove a file path from it. VST3 offers `setDirty` as a hint that the state changed, but nothing obliges a plugin to call it. State calls must be made on the main thread, and a state can be megabytes.

Issue #13's spike (`spikes/plugin-params`, [`README-STATE.md`](../../spikes/plugin-params/README-STATE.md)) built this between members with a test VST3 and measured it. On Windows it then measured 28 real plugins (finding 11) and ran the checklist by hand with Serum 2 and FabFilter Pro-Q 3 (findings 12 and 13). The Mac, another synth by hand, two people editing at the same moment on two machines, and a version pair are still to run; the decision below is provisional until they have.

Serum 2 added three facts the test VST3 could not: every state it loads silences it for about 4 seconds; a knob turn changes its state in a way parameter sync does not reproduce, so the receiver's hash differs and it loads; and it stores an imported wavetable as a path the host cannot see, then shows its own Missing Files dialog when the file is gone.

Pro-Q 3, an effect, showed the other side: it too sends a state for every gesture and rarely hashes the same after parameter sync, but loads it in 0.1 ms with no audible gap. Its display range, a view setting, travels in the state as well.

## Decision

- **What travels.** A plugin's state is the component and controller states as raw bytes, named by the SHA-256 of those bytes, like samples (ADR-0008). Members announce a new state by its hash in an operation in the ordered log (ADR-0004), so a late joiner gets it. The bytes go only to members whose own plugin does not already save that hash, as chunked, resumable binary transfers like samples (`spikes/sample-transfer`), never ahead of live parameter changes.
- **Who wins.** A plugin's state is one value: the newest announcement wins as a whole. There is no merging inside a state.
- **Parameters keep their own order.** A state carries its sender's parameter values and their stamps from when it was read. After loading one, a parameter known to have changed since is set back; a parameter the loaded state holds differently from its sender's snapshot was changed silently by the plugin, counts as written with the state, and is sent on as a parameter change.
- **Finding changes.** In layers: the plugin's hints (`setDirty`, a restart, a burst of parameter reports that change nothing), the mouse let go in the plugin's window, and polling while the window is open, limited to a small share of the main thread and skipped for plugins whose state changes with nothing done. A change is announced once the state has been still for a short time.
- **Loading as little as possible.** A load can interrupt the receiver's sound for seconds, so a member does not load a state for a change that parameter sync already carried, and does not load a burst of states one after another (Serum 2 refused some loads that came too close together). A refused load is retried with the newest state once the plugin has settled. How a sender tells a parameter-only change from a hidden one without seeing inside the state is open; the spike loads whenever hashes differ, and that was too often for Serum 2 but harmless for Pro-Q 3.
- **Loading.** Never while someone on that machine holds a control. Every load is read back and classified (loaded exactly, saved back differently, loaded with different parameters, not loaded), and the last two are shown. Changes during a short settling time after a load are not announced unless a person did something.
- **File paths inside a state** are the plugin's own data. The host detects paths a state names that are missing on this machine and warns; it does not rewrite the state or send the files. This is an exception to "a local file path never goes over the network", limited to opaque plugin state. The host's search sees only plain-text paths and missed Serum 2's; some plugins warn the person themselves. Sending the files is #117.

## Alternatives considered

- **Send the state on every change, with no hash comparison.** Simpler, but every knob move changes the state, so every gesture would send it. Comparing hashes made a knob-only change cost nothing with the test VST3, but not with Serum 2 or Pro-Q 3, whose states rarely matched after parameter sync.
- **Rely on the plugin's `setDirty` only.** Generic, cheap and correct when plugins call it, but nothing requires them to. Kept as one layer, not the only one.
- **Poll the state all the time.** Finds every change, but a read of a 10 MiB state took over 100 ms on the main thread, and polling cannot tell a person's edit from a plugin settling after a load. Kept, but budgeted and limited to while the window is open.
- **Merge states, or diff them.** Impossible for an opaque state without knowing each plugin's format.
- **JUCE's wrapped state, as Tracktion Engine saves it.** It wraps the plugin's bytes in XML with base64, a third larger, and gives no sign whether the plugin accepted a load. Raw bytes, as a `.vstpreset` holds them, are smaller and can be checked.
- **Refuse to sync a state that names a file.** Safer for the path rule, but it would refuse samplers and any plugin that keeps a user file, which is most of what people want synced.

## Consequences

- A preset, an LFO shape or a wavetable follows within about a second when the plugin hints or the person uses its window; a plugin that does neither is found by polling, more slowly, and an edit made in the moments after another member's state arrives can be missed (in the spike's random test, 2.5 to 7 % of runs with no signal at all).
- Two people changing the same plugin's hidden part at once do not merge: one wins. With Serum 2 this once left the two copies showing different LFO shapes although their saved states were equal, which the host cannot detect.
- Each state load can silence the receiving plugin for a few seconds (Serum 2: about 4 s). Until parameter-only changes skip the load, turning a knob in such a plugin interrupts the other person.
- Loading a large state can stall the interface for a moment; the product shows progress and reads large states rarely.
- A plugin version that refuses another version's state leaves the two sounding different. The host can tell and must show it (ADR-0010's flag-or-block question).
- Whatever a plugin saves travels, including how its window looks (Pro-Q 3's display range). Two people cannot keep different views of one plugin.
- A state may carry a person's local paths (and so their user name) to another member.
- The rules are pure and fit the TypeScript sync client beside the parameter rules; the engine host reads, loads, hashes and watches for hints.

## How we will know

Issue #13's remaining runs on the maintainers' machines: the probe on the Mac, the checklist in `README-STATE.md` with another synth, two people editing one plugin at the same moment on two machines, and the version pairs from #14. Then a test of skipping loads for parameter-only changes, with Serum 2. If real plugins rarely save the same bytes for the same content, comparing hashes saves little and every announcement means a transfer. If they rarely hint and the mouse misses their edits, detection needs another layer. If a version pair loads with a silent difference, ADR-0010 should block, not flag, for that case.
