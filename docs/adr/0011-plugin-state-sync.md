# ADR-0011: Plugin state syncs as a content-addressed snapshot, announced by hash

- **Status:** Proposed
- **Date:** 2026-11-15
- **Deciders:** Chad

## Context

Parameter sync (#12) gives two people the same knob values, but many plugins keep part of what makes their sound outside their parameters: Serum 2's LFO shapes, modulation and wavetables, the loaded preset, embedded or referenced samples. Some plugins also change parameters on a preset load without telling the host. PLG-6 asks for this part to sync "by sending the plugin's saved state".

A host cannot see inside a plugin's saved state. It can only ask the plugin for all of it (VST3 `IComponent::getState` and `IEditController::getState`, which JUCE wraps as `getStateInformation`) and give all of it to another copy. It cannot tell what changed, merge two changes, or remove a file path from it. VST3 offers `setDirty` as a hint that the state changed, but nothing obliges a plugin to call it. State calls must be made on the main thread, and a state can be megabytes.

Issue #13's spike (`spikes/plugin-params`, [`README-STATE.md`](../../spikes/plugin-params/README-STATE.md)) built this between members with a test VST3 and measured it. On Windows it then measured 28 real plugins (finding 11) and ran the checklist by hand with Serum 2, FabFilter Pro-Q 3 and Sylenth1 (findings 12 to 14). On a Mac it measured 18 more (finding 15), and with Pro-Q 3 it ran two people editing at the same moment on two machines with two versions of the plugin (findings 16 to 18). Pro-L 2 on two machines and the fix for finding 18 are still to do; the decision below is provisional until they are.

Serum 2 added two facts the test VST3 could not: every state it loads silences it for about 4 seconds, and it stores an imported wavetable as a path the host cannot see, then shows its own Missing Files dialog when the file is gone. Pro-Q 3, an effect, loads a state in 0.1 ms with no audible gap, and its display range, a view setting, travels in the state. Sylenth1 does not report a preset's parameters to the host, and hints before its state holds the new preset.

At first every knob turn in all three loaded a state on the receiver. That was the spike's wire format rounding parameter values (finding 14). With values sent exactly, a knob turn left both copies saving the same bytes, and nothing loaded.

Between a Windows PC and a Mac, two people editing Pro-Q 3 at the same moment ended on the same state every time: different bands both kept, the same band won by whoever let go last, a preset won whole. Pro-Q 3 3.2.7.0 and 3.2.9.0 loaded each other's states. But one drag was undone on both machines: a state read halfway through it was announced with the parameters from after it, and the receiver sent the halfway values back as the plugin's own silent change (finding 18, a spike bug). Three plugins on the Mac keep their whole state in the controller state.

## Decision

- **What travels.** A plugin's state is the component and controller states as raw bytes (both: some plugins keep everything in the controller state), named by the SHA-256 of those bytes, like samples (ADR-0008). Members announce a new state by its hash in an operation in the ordered log (ADR-0004), so a late joiner gets it. The bytes go only to members whose own plugin does not already save that hash, as chunked, resumable binary transfers like samples (`spikes/sample-transfer`), never ahead of live parameter changes.
- **Who wins.** A plugin's state is one value: the newest announcement wins as a whole. There is no merging inside a state. A state from someone else that arrives while a change here has not gone out yet replaces it: it loads once nobody here holds a control, and the parameters changed here since its sender read it, the held one included, are set back on top. A person turning a knob while another member changes the preset keeps the knob; everything else follows the preset (decided 20 November 2026). A hidden edit here that had not gone out is lost to it; whether to tell the person is open. A copy never answers an incoming state by announcing its own.
- **Parameters keep their own order.** A state carries its sender's parameter values and their stamps from when it was read. After loading one, a parameter known to have changed since is set back; a parameter the loaded state holds differently from its sender's snapshot was changed silently by the plugin, counts as written with the state, and is sent on as a parameter change.
- **Finding changes.** In layers: the plugin's hints (`setDirty`, a restart, a burst of parameter reports that change nothing), the mouse let go in the plugin's window, and polling while the window is open, limited to a small share of the main thread and skipped for plugins whose state changes with nothing done. A change is announced once the state has been still for a short time, with bytes and parameter values read together after the parameters are still, for every plugin. (A further limit, that a loaded state's value never wins over a newer change its own sender made to the same parameter, was considered and not built: it would also hide real silent changes, and reading bytes and parameters together removes the stale value it guarded against.)
- **Loading as little as possible.** A load can interrupt the receiver's sound for seconds, so a member does not load a state for a change that parameter sync already carried. Parameter values travel exactly, bit for bit, so that such a change leaves both plugins saving the same bytes and the hash comparison skips the load. A member does not load a burst of states one after another (Serum 2 refused some loads that came too close together), and retries a refused load with the newest state once the plugin has settled.
- **Reading after a hint.** The state is read when a hint arrives and again shortly after, since a plugin can hint before its state holds the change.
- **Loading.** Never while someone on that machine holds a control. Every load is read back and classified (loaded exactly, saved back differently, loaded with different parameters, not loaded), and the last two are shown. A load that changes nothing because the plugin already held that content, with every parameter equal to the sender's, is not a refusal; between versions this is common. Changes during a short settling time after a load are not announced unless a person did something.
- **File paths inside a state** are the plugin's own data. The host detects paths a state names that are missing on this machine and warns; it does not rewrite the state or send the files. This is an exception to "a local file path never goes over the network", limited to opaque plugin state. The host's search sees only plain-text paths and missed Serum 2's; some plugins warn the person themselves. Sending the files is #117.

## Alternatives considered

- **Send the state on every change, with no hash comparison.** Simpler, but every knob move changes the state, so every gesture would send it. Comparing hashes made a knob-only change cost nothing with the test VST3, Serum 2, Pro-Q 3 and Sylenth1, once parameter values travelled exactly.
- **Rely on the plugin's `setDirty` only.** Generic, cheap and correct when plugins call it, but nothing requires them to. Kept as one layer, not the only one.
- **Poll the state all the time.** Finds every change, but a read of a 10 MiB state took over 100 ms on the main thread, and polling cannot tell a person's edit from a plugin settling after a load. Kept, but budgeted and limited to while the window is open.
- **Merge states, or diff them.** Impossible for an opaque state without knowing each plugin's format.
- **JUCE's wrapped state, as Tracktion Engine saves it.** It wraps the plugin's bytes in XML with base64, a third larger, and gives no sign whether the plugin accepted a load. Raw bytes, as a `.vstpreset` holds them, are smaller and can be checked.
- **Refuse to sync a state that names a file.** Safer for the path rule, but it would refuse samplers and any plugin that keeps a user file, which is most of what people want synced.

## Consequences

- A preset, an LFO shape or a wavetable follows within about a second when the plugin hints or the person uses its window; a plugin that does neither is found by polling, more slowly, and an edit made in the moments after another member's state arrives, with no sign of a person, can be taken as the plugin settling (2 to 4.5 % of the spike's simulated runs with only `setDirty` or no signal at all; none when a person's input is seen).
- Two people changing the same plugin's hidden part at once do not merge: one wins. A hidden edit in progress is lost to a state that arrives during it, and the person is not told (open). With Serum 2 this once left the two copies showing different LFO shapes although their saved states were equal, which the host cannot detect.
- Each state load can silence the receiving plugin for a few seconds (Serum 2: about 4 s). A knob turn does not load, but a preset or another hidden change does, and the other person hears the gap.
- The parameter wire format is bound to exactness: a format that rounds values makes every knob turn a state load.
- Loading a large state can stall the interface for a moment; the product shows progress and reads large states rarely.
- A plugin version that refuses another version's state leaves the two sounding different. The host can tell and must show it (ADR-0010's flag-or-block question). The one real pair tried, Pro-Q 3 3.2.7.0 and 3.2.9.0, synced both ways, which supports flagging over blocking.
- Two people on the same control: the last to let go wins, and the other sees their value replaced. Two people on different controls of one plugin both keep their edits.
- A knob turned while someone else's state arrives can make that person's state lose, since the turner's copy counts its own changed state as a competing hidden change. Loading the newcomer and putting the newer parameters back would keep both, at the cost of mixing them; presence or a soft lock per plugin might make it rare. Open (`README-STATE.md`, "Open: a knob turned while someone else's state arrives").
- Whatever a plugin saves travels, including how its window looks (Pro-Q 3's display range). Two people cannot keep different views of one plugin.
- A state may carry a person's local paths (and so their user name) to another member.
- The rules are pure and fit the TypeScript sync client beside the parameter rules; the engine host reads, loads, hashes and watches for hints.

## How we will know

Issue #13's remaining runs on the maintainers' machines: Pro-L 2 on two machines, and the band drag repeated now that finding 18 is fixed in the rules. Further version pairs from #14 as they come. If real plugins rarely save the same bytes for the same content, comparing hashes saves little and every announcement means a transfer. If they rarely hint and the mouse misses their edits, detection needs another layer. If a version pair loads with a silent difference, ADR-0010 should block, not flag, for that case.
