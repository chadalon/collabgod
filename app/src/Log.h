// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace collabgod
{
    /** Starts writing the app's log: standard output, and a file in the system's log
        folder (~/Library/Logs/CollabGod on macOS, %APPDATA%\CollabGod on Windows). */
    void startLog();

    /** Stops writing the log file. */
    void stopLog();

    /** Where the log file is, once startLog has been called. */
    juce::File logFile();

    /** Writes one line to the log, with the time. Message thread only. */
    void log (const juce::String& message);

    /** Turns log() off while it exists, for tests that make the app log on purpose. */
    struct ScopedSilentLog
    {
        ScopedSilentLog();
        ~ScopedSilentLog();
    };
}
