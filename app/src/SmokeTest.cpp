// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SmokeTest.h"

#include <iostream>

namespace te = tracktion;

namespace collabgod
{
    namespace
    {
        /** No audio device, so the test runs on CI machines that have none. */
        struct HeadlessBehaviour : te::EngineBehaviour
        {
            bool autoInitialiseDeviceManager() override { return false; }
        };

        bool expect (bool condition, const juce::String& what)
        {
            std::cout << (condition ? "PASS " : "FAIL ") << what << std::endl;
            return condition;
        }
    }

    juce::String versionLine()
    {
        return juce::String (JUCE_APPLICATION_NAME_STRING) + " " + JUCE_APPLICATION_VERSION_STRING
             + " (" + juce::SystemStats::getJUCEVersion()
             + ", Tracktion Engine " + juce::String (COLLABGOD_TRACKTION_COMMIT).substring (0, 12) + ")";
    }

    int runSmokeTest()
    {
        std::cout << versionLine() << std::endl;

        te::Engine engine ("CollabGod", nullptr, std::make_unique<HeadlessBehaviour>());

        auto edit = te::Edit::createSingleTrackEdit (engine, te::Edit::forEditing);
        bool ok = expect (edit != nullptr, "an edit was created");

        if (edit != nullptr)
        {
            edit->ensureNumberOfAudioTracks (2);
            ok = expect (te::getAudioTracks (*edit).size() == 2, "the edit holds two audio tracks") && ok;
            ok = expect (! edit->getTransport().isPlaying(), "the transport starts stopped") && ok;
        }

        edit.reset();
        std::cout << (ok ? "Smoke test passed" : "Smoke test FAILED") << std::endl;
        return ok ? 0 : 1;
    }
}
