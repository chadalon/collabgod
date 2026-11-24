// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace collabgod
{
    /** One line naming the app, its version and the engine it was built against. */
    juce::String versionLine();

    /** Checks that the linked engine works without a window or an audio device:
        starts Tracktion Engine, makes an edit with two audio tracks and checks it.
        Prints what it did. Returns the process exit code: 0 when it passed. */
    int runSmokeTest();
}
