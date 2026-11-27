// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace collabgod
{
    /** Tests of the atomic write (`CollabGod --project-file-test`, run by ctest): writing,
        replacing, failing without touching the target, and a writer killed at random moments
        while it saves, again and again, never leaving a damaged file. Prints what failed.
        Returns the process exit code: 0 when every check passed. */
    int runProjectFileTests();

    /** The writer the crash test starts and kills: saves `target` over and over, each time a
        whole new generation, until it is killed. Never returns normally. */
    int runCrashWriter (const juce::File& target);
}
