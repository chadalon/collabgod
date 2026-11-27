// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace collabgod
{
    /** Tests of audio output (`CollabGod --audio-test`, run by ctest), on simulated sound cards
        and never the machine's own: outputs only, choosing and remembering a device, rate and
        block size, sound reaching the device once the engine has settled, late callbacks,
        exclusive mode's block size, the audio thread setting, and devices unplugged and plugged
        in again, with the test calling the audio callback block by block and with a device
        running in real time. Prints what failed. Returns the process exit code: 0 when every
        check passed. Message thread. */
    int runAudioTests();
}
