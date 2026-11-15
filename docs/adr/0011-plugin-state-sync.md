# ADR-0011: Plugin state syncs as a content-addressed snapshot, announced by hash

- **Status:** Proposed
- **Date:** 2026-11-15
- **Deciders:** Chad

## Context

Parameter sync (#12) gives two people the same knob values, but many plugins keep part of what makes their sound outside their parameters: Serum 2's LFO shapes, modulation and wavetables, the loaded preset, embedded or referenced samples. Some plugins also change parameters on a preset load without telling the host. PLG-6 asks for this part to sync "by sending the plugin's saved state".

A host cannot see inside a plugin's saved state. It can only ask the plugin for all of it (VST3 `IComponent::getState` and `IEditController::getState`, which JUCE wraps as `getStateInformation`) and give all of it to another copy. It cannot tell what changed, merge two changes, or remove a file path from it. VST3 offers `setDirty` as a hint that the state changed, but nothing obliges a plugin to call it. State calls must be made on the main thread, and a state can be megabytes.

Issue #13's spike (`spikes/plugin-params`, [`README-STATE.md`](../../spikes/plugin-params/README-STATE.md)) built this between members with a test VST3 and measured it. It has not yet been run with real plugins; the decision below is provisional until it has.

## Decision

- **What travels.** A plugin's state is the component and controller states as raw bytes, named by the SHA-256 of those bytes, like samples (ADR-0008). Members announce a new state by its hash in an operation in the ordered log (ADR-0004), so a late joiner gets it. The bytes go only to members whose own plugin does not already save that hash, as chunked, resumable binary transfers like samples (`spikes/sample-transfer`), never ahead of live parameter changes.
- **Who wins.** A plugin's state is one value: the newest announcement wins as a whole. There is no merging inside a state.
- **Parameters keep their own order.** A state carries its sender's parameter values and their stamps from when it was read. After loading one, a parameter known to have changed since is set back; a parameter the loaded state holds differently from its sender's snapshot was changed silently by the plugin, counts as written with the state, and is sent on as a parameter change.
- **Finding changes.** In layers: the plugin's hints (`setDirty`, a restart, a burst of parameter reports that change nothing), the mouse let go in the plugin's window, and polling while the window is open, limited to a small share of the main thread and skipped for plugins whose state changes with nothing done. A change is announced once the state has been still for a short time.
- **Loading.** Never while someone on that machine holds a control. Every load is read back and classified (loaded exactly, saved back differently, loaded with different parameters, not loaded), and the last two are shown. Changes during a short settling time after a load are not announced unless a person did something.
- **File paths inside a state** are the plugin's own data. The host detects paths a state names that are missing on this machine and warns; it does not rewrite the state or send the files. This is an exception to "a local file path never goes over the network", limited to opaque plugin state.

## Alternatives considered

- **Send the state on every change, with no hash comparison.** Simpler, but every knob move changes the state, so every gesture would send it. Comparing hashes made a knob-only change cost nothing in the spike.
- **Rely on the plugin's `setDirty` only.** Generic, cheap and correct when plugins call it, but nothing requires them to. Kept as one layer, not the only one.
- **Poll the state all the time.** Finds every change, but a read of a 10 MiB state took over 100 ms on the main thread, and polling cannot tell a person's edit from a plugin settling after a load. Kept, but budgeted and limited to while the window is open.
- **Merge states, or diff them.** Impossible for an opaque state without knowing each plugin's format.
- **JUCE's wrapped state, as Tracktion Engine saves it.** It wraps the plugin's bytes in XML with base64, a third larger, and gives no sign whether the plugin accepted a load. Raw bytes, as a `.vstpreset` holds them, are smaller and can be checked.
- **Refuse to sync a state that names a file.** Safer for the path rule, but it would refuse samplers and any plugin that keeps a user file, which is most of what people want synced.

## Consequences

- A preset, an LFO shape or a wavetable follows within about a second when the plugin hints or the person uses its window; a plugin that does neither is found by polling, more slowly, and an edit made in the moments after another member's state arrives can be missed (in the spike's random test, 2.5 to 7 % of runs with no signal at all).
- Two people changing the same plugin's hidden part at once do not merge: one wins.
- Loading a large state can stall the interface for a moment; the product shows progress and reads large states rarely.
- A plugin version that refuses another version's state leaves the two sounding different. The host can tell and must show it (ADR-0010's flag-or-block question).
- A state may carry a person's local paths (and so their user name) to another member.
- The rules are pure and fit the TypeScript sync client beside the parameter rules; the engine host reads, loads, hashes and watches for hints.

## How we will know

Issue #13's remaining runs on the maintainers' machines: the probe (sizes and behaviour of every installed plugin, including Serum 2) and the checklist in `README-STATE.md` with Serum 2, another synth, an effect, a plugin that names a file, and the version pairs from #14. If real plugins rarely save the same bytes for the same content, comparing hashes saves little and every announcement means a transfer. If they rarely hint and the mouse misses their edits, detection needs another layer. If a version pair loads with a silent difference, ADR-0010 should block, not flag, for that case.
