# ADR-0018: How the app opens and keeps its audio output

- **Status:** Proposed
- **Date:** 2026-11-27
- **Deciders:** Chad

## Context

#27 makes sound come out of both maintainer machines: a person chooses the driver type, output device, sample rate and block size, the app remembers them, and unplugging or replugging the device must not crash it. The Phase 0 spikes found five things the app has to do differently from Tracktion Engine's and JUCE's defaults:

- The spikes opened the default device with its inputs, and macOS asked for the microphone (`engine-toolchain` and `webview-ui`, troubleshooting). v1 does not record.
- Tracktion's default thread pool (every CPU) made audio callbacks late during edits on the 4-core Intel Mac, and one thread did not (`engine-projection` findings 5 and 13). #113 is still looking for the cause.
- CoreAudio counts no dropouts, and a late callback still makes clean audio, so neither the device nor a check of the output sees one (`engine-projection` finding 13).
- Windows Audio in exclusive mode dropped out constantly at its default block size (`engine-projection` finding 12).
- Play is lost if it comes before the engine has settled after the device starts (`engine-toolchain` finding 8, `webview-ui` finding 8).

Tracktion and JUCE also keep the device setup in a way that does not fit a person's choice. JUCE's `AudioDeviceManager` falls back to the default device when the chosen one is unplugged, but does not go back to the chosen one when it returns, and does nothing when no device was open at all. Tracktion saves whatever is open to its own settings, so a fallback would overwrite the choice.

## Decision

`EngineHost` (`app/src/EngineHost.h`) owns the engine and the output, and:

1. **Opens outputs only.** Every device setup it gives JUCE names no input device and no input channels. The microphone permission text stays in the app's `Info.plist`, because macOS ends an app that touches an input without one, but it should never be shown.
2. **Keeps one record of the choice**, the `audioOutput` entry in the app's own `settings.xml`, which Tracktion's settings (its own `Settings.xml`, in an `Engine` folder beside it, since Windows and macOS do not tell the two names apart) read from and never write to. A fallback device is never saved over it.
3. **Falls back, then goes back.** When the chosen device goes away it plays through another output and says so; when the chosen device is listed again it opens it again, at the chosen rate and block size. With no device at all it opens the first one that appears. A driver type that does not announce devices coming back (ASIO, DirectSound) is scanned every 2 s while the chosen device is missing.
4. **Never makes exclusive mode the default** (JUCE's first type with a device, Windows Audio in shared mode, is), and in exclusive mode uses the smallest block of at least 10 ms unless a block size was chosen.
5. **Uses one audio thread by default**, as a setting a person can change (and `--audio-threads` for a run), until #113 says whether a pool is safe on small machines.
6. **Times every audio callback itself** and counts the ones that took longer than their block, beside whatever the driver counts.
7. **Holds back anything that starts playback until the engine has settled** (`whenSettled`), with the rule the `webview-ui` spike arrived at, and starts settling again whenever the device changes.
8. **Opens ASIO from the ASIO SDK headers JUCE bundles under the GPLv3** (`JUCE_ASIO=1` on Windows). Steinberg has offered the ASIO SDK under the GPLv3 since October 2025, and JUCE 8.0.11 started shipping it; it is listed in `THIRD_PARTY.md`.

## Alternatives considered

- **Let Tracktion save the device setup**, as it does by default. A fallback device would replace the choice, and the chosen device would not come back after it was replugged.
- **JUCE's own device selector** (`AudioDeviceSelectorComponent`) in a native window. Quick, but it changes the device manager directly, bypassing the remembered choice, offers inputs, and is not the app's interface. The page's panel is plain HTML until the design system (#33) restyles it.
- **Count late callbacks by the time between callbacks.** Drivers that buffer (WASAPI shared mode) call back in bursts, which would look late when nothing was wrong. Timing the callback itself does not.
- **JUCE's own count (`AudioDeviceManager::getXRunCount`)**, which the spike used. It is reset whenever the device restarts, so it cannot count across a fallback; the app's own timer keeps counting.
- **Leave exclusive mode at its default block size.** It dropped out constantly on Chad's machine.

## Consequences

- The timer relies on the order in which JUCE calls its callbacks (the first, then the rest from the last to the second). `--audio-test` fails if that changes: a slow callback would no longer be counted.
- Tracktion's callback now writes into JUCE's temporary buffer, which JUCE adds to the device's. That is one more pass over each block, a few microseconds.
- A new choice is opened with `AudioDeviceManager::initialise` and the whole device setup at once, so a change of driver type opens the new device straight away. JUCE's own `setCurrentAudioDeviceType` would first open the new type's default device and wait 1.5 s; if two drivers turn out to clash when switched quickly (JUCE's reason for that wait, with DirectSound and ASIO), that is where to look. `initialise` compares only device names, rate and block size, not the type, so a device with the same name under another type (Windows Audio and its exclusive mode list the same devices) would leave the old type's device open; the app closes the device first whenever the type changes.
- One audio thread may not be enough under plugin load. #113, and the plugin checks in #27's manual steps, decide the default; it is one function, `defaultAudioThreads`.
- The app's name for a driver type is JUCE's ("Windows Audio", "Windows Audio (Exclusive Mode)", "ASIO", "CoreAudio"). A remembered choice is matched by those names, so a JUCE update that renames a type would lose the choice once.
- The ASIO name appears in the driver list. Using the ASIO logo would mean following Steinberg's usage guidelines; the app does not use it.

## How we will know

The manual steps in #27's pull request, on Chad's Windows desktop (Windows Audio, exclusive mode and ASIO, with the Focusrite) and his Intel Mac (CoreAudio), with the device unplugged and replugged while playing, and other audio apps open. #113 settles the thread count.
