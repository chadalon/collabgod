# ADR-0010: A plugin is identified by its VST3 class ID, scanned out of process

- **Status:** Proposed
- **Date:** 2026-11-14
- **Deciders:** Chad

## Context

A lobby may only use a plugin that every member has (PLG-2, PLG-3), and should warn when the installs are different versions (PLG-7). So the app needs a rule for when two machines have "the same plugin", and a way to find out what each machine has without a misbehaving plugin taking the app down.

A VST3 module exports a class factory. Each class has a 16-byte class ID, a category (the plugin itself is the "Audio Module Class"), a name, a vendor and a version text. The class ID is what a host uses to create the plugin and what a saved project refers to. Names change between releases and are written differently by different builds; vendors rename themselves. Versions are less reliable still: vendors do not always update the version the factory reports, and the Windows and Mac builds of one release can report it differently (the comment on #14). A bundle can also carry a version outside the factory (Info.plist on macOS, the version resource on Windows) and, since VST3 3.7.5, a `moduleinfo.json` that lists the classes and the older class IDs each one replaces.

Reading a factory means loading the plugin's code. Plugins crash, hang on licence dialogs, and occasionally crash only when unloaded. Issue #14's spike (`spikes/plugin-inventory`) built a scanner and a comparison tool to test this against both maintainers' plugin folders.

## Decision

**Identity.** Two installs are the same plugin when the canonical text form of their Audio Module Class ID is equal: the ID's four 32-bit words as 32 upper-case hex digits, which is what Steinberg's `FUID::toString` prints on every system. The raw 16 bytes are not compared, because on Windows they are stored in COM GUID byte order and elsewhere big-endian.

**Version.** Given the same class ID, the class version text the factory reports decides:

- the same text, or texts equal once trimmed and with trailing `.0` parts removed (`1.2` and `1.2.0.0`): identical;
- different: usable but flagged before use (PLG-7). Whether this should instead block the plugin is left open until #12 (parameter sync) and #13 (state sync) show whether a version difference breaks anything. It is the maintainer's call; the comparison tool takes it as a setting. The maintainer's current lean (2026-11-15) is to flag, not block: in the first real comparison all 9 plugins shared by class ID differed in version, so blocking would have refused every one of them. This is a lean, not a decision, and stays open until #12 and #13 report what a version difference does to parameters and saved state.

A different class ID is a different plugin, even with the same name. A class ID that `moduleinfo.json` says replaces another is reported, not treated as the same plugin.

**Scanning.** Each bundle is loaded in its own child process with a time limit (30 seconds by default). A crash or hang is recorded against that bundle and the scan moves on. The child writes its result before it releases the plugin, so a crash on unload still leaves a usable result, recorded as a problem.

**What is exchanged.** On join, members exchange the class ID, name, vendor and version of each plugin, never a path or a plugin file.

## Alternatives considered

- **Vendor and name.** Readable and often stable, but not what a host or a project uses, and two different classes can share a name. The comparison reports same-name, different-ID pairs so the real inventories can show whether names and IDs ever disagree.
- **JUCE's plugin identifier string.** Tracktion Engine stores plugins by JUCE's `PluginDescription`, whose identifier string is built from the name, a hash of the file's location and a number derived from the class ID. Locations differ between machines and systems, so the string does too. Whatever the engine stores locally, the identity exchanged between machines is the class ID.
- **`moduleinfo.json` alone.** Lets a host identify plugins without loading them, but many plugins do not ship it, and it can disagree with the factory. The scanner records it beside the factory's answer.
- **The bundle's own version (Info.plist, version resource) instead of the factory's.** It is what the installer shows, but it is missing for some plugins, is not the same field on the two systems, and is not what the plugin reports to a host. The comparison tool reports where the two disagree.
- **Scanning in the app's process**, as hosts often do on a first run. Faster, but one bad plugin crashes the app, which is the case PLG-8 later rules out at run time too.

## Consequences

- The lobby's shared-plugin check is a set intersection on class IDs, with a version comparison on each match. Both are pure and fit the TypeScript session model.
- Two editions of a product with different class IDs never match, even if one could open the other's state. If the real inventories show this matters, `moduleinfo.json`'s compatibility list is the way to relate them, as a later change.
- A plugin whose factory reports a stale version can be flagged as matching when it does not, or as different when it is the same release. The comparison tool lists both cases so the policy can be set from data.
- Scanning costs a process start per bundle (a few milliseconds) on top of the plugin's own load time. A product scanner should cache by bundle path and modified time and rescan only what changed, like the sample index (ADR-0008).
- A plugin reference in the session names its format as well as its ID (`vst3` in v1), so another format can be added later without changing the session format. Cross-format matching (AU or CLAP against VST3) is parked in #82; AAX cannot be hosted by third parties at all.
- A plugin that fails to scan on one machine is not available in a lobby there, even if it would load in the engine. The scan result says why, so the interface can tell the person.

## How we will know

Issue #14: both maintainers scan their real plugin folders (Windows desktop and the Intel Mac) and compare the inventories. The rule holds if plugins both own come out as shared, and any same-name, different-ID pairs or version disagreements are explained. Issues #12 and #13 then decide whether a version mismatch is flagged or blocked.

Result of the #14 scans (2026-11-14, Windows 11 and macOS 13, VST3 only): 9 plugins shared by class ID, 12 only on Windows, 7 only on the Mac. Class IDs matched across the two systems. Names and vendors did not always match (the Mac reported "FabFilter FabFilter Pro-C 2" and "iZotope, Inc."), and the bundle version was unreliable (Windows adds a build number, macOS FabFilter bundles drop the dots, two Focusrite bundles say `0.0.0`), so the factory's class version is the one to compare. All 9 shared plugins had different versions. The rule held.

Issue #13's spike (2026-11-15, cloud only) adds what a version difference does to saved state: with its test plugin, the older version refused the newer one's state without any sign to the host, so the two copies went on sounding different; the host noticed only by reading the state back after the load. Its harness classifies every load this way, so the real version pairs from #14 can be checked the same way (`spikes/plugin-params/README-STATE.md`, finding 9). Until they are, the spike's suggestion is to flag at join and warn on each load that is refused or differs (ADR-0011), not to block.
