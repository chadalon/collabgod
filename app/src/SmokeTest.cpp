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

        /** Tracktion's settings in a folder of the test's own. Its default is Settings.xml in
            the app's data folder, which on Windows and macOS is the same file as the app's own
            settings.xml, so every test run wrote Tracktion's keys into a person's settings. */
        struct TemporaryStorage : te::PropertyStorage
        {
            explicit TemporaryStorage (const juce::File& f) : te::PropertyStorage ("CollabGod"), folder (f) {}
            juce::File getAppPrefsFolder() override { return folder; }

            const juce::File folder;
        };

        bool expect (bool condition, const juce::String& what)
        {
            std::cout << (condition ? "PASS " : "FAIL ") << what << std::endl;
            return condition;
        }

        bool runChecks (te::Engine& engine)
        {
            auto edit = te::Edit::createSingleTrackEdit (engine, te::Edit::forEditing);
            bool ok = expect (edit != nullptr, "an edit was created");

            if (edit != nullptr)
            {
                edit->ensureNumberOfAudioTracks (2);
                ok = expect (te::getAudioTracks (*edit).size() == 2, "the edit holds two audio tracks") && ok;
                ok = expect (! edit->getTransport().isPlaying(), "the transport starts stopped") && ok;
            }

            return ok;
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

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("collabgod-smoke-test", {}, false);
        folder.createDirectory();

        bool ok = false;
        {
            te::Engine engine (std::make_unique<TemporaryStorage> (folder), nullptr, std::make_unique<HeadlessBehaviour>());
            ok = runChecks (engine);
        }

        folder.deleteRecursively();
        std::cout << (ok ? "Smoke test passed" : "Smoke test FAILED") << std::endl;
        return ok ? 0 : 1;
    }
}
