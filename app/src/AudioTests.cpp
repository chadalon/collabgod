// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `CollabGod --audio-test`. No sound card: every device here is a SimulatedAudioDevice in the
// real juce::AudioDeviceManager and Tracktion DeviceManager, so the app's own code runs on the
// path a real device takes. Where the test calls the audio callback itself, it does so from a
// thread of its own, as a driver's audio thread would, never the message thread.

#include "AudioTests.h"

#include "AudioSettings.h"
#include "EngineHost.h"
#include "Log.h"
#include "SimulatedAudio.h"

#include <iostream>
#include <thread>

namespace te = tracktion;

namespace collabgod
{
    namespace
    {
        struct Results
        {
            int passed = 0;
            int failed = 0;

            bool expect (bool condition, const juce::String& what)
            {
                if (condition)
                    ++passed;
                else
                {
                    ++failed;
                    std::cout << "FAIL " << what << std::endl;
                }
                return condition;
            }
        };

        /** A folder of its own for each host's settings, deleted afterwards. */
        struct TempFolder
        {
            TempFolder() : folder (juce::File::createTempFile ("collabgod-audio-test")) { folder.createDirectory(); }
            ~TempFolder() { folder.deleteRecursively(); }
            juce::File folder;
        };

        /** Runs the message loop (timers, change messages) until `done` or `timeoutMs`. */
        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 5000)
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
            while (! done())
            {
                if (juce::Time::getMillisecondCounter() > end)
                    return false;
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            }
            return true;
        }

        void pumpFor (int ms)
        {
            pumpUntil ([] { return false; }, ms);
        }

        /** Calls the device's audio callback `blocks` times from a thread that plays the
            driver's audio thread. Returns how many blocks were rendered. */
        int renderOnAudioThread (SimulatedAudioDevice* device, int blocks)
        {
            if (device == nullptr)
                return 0;
            int rendered = 0;
            std::thread audio ([&] { rendered = device->render (blocks); });
            audio.join();
            return rendered;
        }

        struct Fixture
        {
            explicit Fixture (const juce::File& folder,
                              SimulatedAudioDeviceType::Clock clock = SimulatedAudioDeviceType::Clock::manual,
                              juce::String typeName = SimulatedAudioDeviceType::typeName,
                              int defaultBlock = 441,
                              int threadsOverride = 0,
                              juce::String otherTypeName = {})
            {
                const juce::StringArray names { "Simulated Output 1", "Simulated Output 2" };
                auto t = std::make_unique<SimulatedAudioDeviceType> (clock, names, typeName, defaultBlock);
                type = t.get();

                EngineHost::Options options;
                options.settingsFolder = folder;
                options.systemDevices = false;
                options.extraTypes.push_back (std::move (t));

                // A second type listing the same devices, as Windows Audio and its exclusive mode do.
                if (otherTypeName.isNotEmpty())
                {
                    auto o = std::make_unique<SimulatedAudioDeviceType> (clock, names, otherTypeName, defaultBlock);
                    other = o.get();
                    options.extraTypes.push_back (std::move (o));
                }
                options.audioThreadsOverride = threadsOverride;
                options.watchForStalls = clock == SimulatedAudioDeviceType::Clock::realTime;
                host = std::make_unique<EngineHost> (std::move (options));
            }

            SimulatedAudioDevice* openDevice() const
            {
                if (auto* d = host->getDeviceManager().getCurrentAudioDevice())
                    return type->findOpenDevice (d->getName());
                return nullptr;
            }

            bool settle (int timeoutMs = 6000) { return pumpUntil ([this] { return host->isSettled(); }, timeoutMs); }

            SimulatedAudioDeviceType* type = nullptr; // owned by the device manager
            SimulatedAudioDeviceType* other = nullptr; // likewise; only with otherTypeName
            std::unique_ptr<EngineHost> host;
        };

        //==============================================================================
        void testSettings (Results& r)
        {
            AudioChoice c { "Windows Audio", "Focusrite USB", 48000.0, 256, 2 };
            const auto xml = c.toXml();
            r.expect (AudioChoice::fromXml (xml.get()) == c, "a choice survives XML");

            juce::XmlElement bad ("AUDIO_OUTPUT");
            bad.setAttribute ("sampleRate", -5.0);
            bad.setAttribute ("bufferSize", -1);
            bad.setAttribute ("audioThreads", -3);
            const auto read = AudioChoice::fromXml (&bad);
            r.expect (juce::exactlyEqual (read.sampleRate, 0.0) && read.bufferSize == 0 && read.audioThreads == 0,
                      "negative numbers in the settings file read as defaults");
            r.expect (AudioChoice::fromXml (nullptr) == AudioChoice {}, "no settings means every default");

            const auto setup = c.toDeviceSetupXml();
            r.expect (setup->getStringAttribute ("audioInputDeviceName").isEmpty() && setup->getStringAttribute ("audioDeviceInChans") == "0",
                      "the device setup names no input device and no input channels");
            r.expect (setup->getStringAttribute ("audioOutputDeviceName") == "Focusrite USB" && setup->getIntAttribute ("audioDeviceBufferSize") == 256,
                      "the device setup names the output and its block size");
            r.expect (! AudioChoice { "CoreAudio", "MacBook Pro Speakers", 0.0, 0, 0 }.toDeviceSetupXml()->hasAttribute ("audioDeviceRate"),
                      "an unchosen rate is left to the device");

            r.expect (defaultAudioThreads (4) == 1 && defaultAudioThreads (32) == 1, "one audio thread by default until #113");
            r.expect (isExclusiveModeType ("Windows Audio (Exclusive Mode)") && ! isExclusiveModeType ("Windows Audio")
                          && ! isExclusiveModeType ("Windows Audio (Low Latency Mode)") && ! isExclusiveModeType ("ASIO"),
                      "only exclusive mode is exclusive");
            r.expect (safeExclusiveBlockSize ({ 32, 64, 128, 448, 960 }, 44100.0, 32) == 448, "exclusive mode: the smallest block of 10 ms or more");
            r.expect (safeExclusiveBlockSize ({ 32, 64, 128 }, 48000.0, 32) == 128, "exclusive mode: the largest when none is 10 ms");
            r.expect (safeExclusiveBlockSize ({}, 48000.0, 96) == 96, "exclusive mode: the current size when the device lists none");

            TempFolder temp;
            {
                SettingsFile file (temp.folder);
                r.expect (file.audioChoice() == AudioChoice {}, "a new settings file chooses nothing");
                file.setAudioChoice (c);
            }
            r.expect (SettingsFile (temp.folder).audioChoice() == c, "the settings file remembers the choice at once");
        }

        void testOutputsOnly (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            Fixture f (temp.folder);

            auto* device = f.openDevice();
            r.expect (device != nullptr, "with nothing chosen, the default output opens");
            if (device == nullptr)
                return;

            r.expect (device->getName() == "Simulated Output 1", "the default is the type's default device");
            r.expect (! device->inputDeviceWasRequested() && device->getActiveInputChannels().isZero(),
                      "no input is opened, so macOS would not ask for the microphone");
            r.expect (device->getActiveOutputChannels().countNumberOfSetBits() == 2, "both outputs are opened");
            r.expect (f.type->numOpenDevices() == 1, "one device is open");

            const auto s = f.host->status();
            r.expect (s.state == AudioState::open && s.type == SimulatedAudioDeviceType::typeName && s.bufferSize == 441
                          && juce::exactlyEqual (s.sampleRate, 44100.0),
                      "the status shows the open device: " + toString (s.state) + " " + s.device);
            r.expect (s.sampleRates.size() == 3 && s.bufferSizes.contains (256), "the status lists the device's rates and block sizes");
        }

        void testChooseAndRemember (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            const AudioChoice wanted { SimulatedAudioDeviceType::typeName, "Simulated Output 2", 48000.0, 256, 1 };

            {
                Fixture f (temp.folder);
                const auto s = f.host->choose (wanted);
                r.expect (s.state == AudioState::open && s.device == "Simulated Output 2", "choosing a device opens it: " + s.message);
                r.expect (juce::exactlyEqual (s.sampleRate, 48000.0) && s.bufferSize == 256, "at the chosen rate and block size");
                r.expect (s.chosen == wanted, "and remembers it");
                r.expect (f.type->numOpenDevices() == 1, "the old device was closed");
                auto* device = f.openDevice();
                r.expect (device != nullptr && ! device->inputDeviceWasRequested(), "a chosen device is opened with outputs only too");
            }

            r.expect (SettingsFile (temp.folder).audioChoice() == wanted, "the choice is in the settings file");
            // On Windows and macOS file names are not case-sensitive: Tracktion's Settings.xml
            // once overwrote the app's settings.xml.
            juce::StringArray files;
            for (const auto& file : temp.folder.findChildFiles (juce::File::findFiles, false))
                files.add (file.getFileName());
            r.expect (files == juce::StringArray { "settings.xml" }, "the engine's settings are kept apart from the app's: "
                                                                        + files.joinIntoString (", "));
            r.expect (temp.folder.getChildFile ("Engine").isDirectory(), "in a folder of their own");

            {
                Fixture f (temp.folder);
                const auto s = f.host->status();
                r.expect (s.state == AudioState::open && s.device == "Simulated Output 2" && juce::exactlyEqual (s.sampleRate, 48000.0)
                              && s.bufferSize == 256,
                          "the next start opens the remembered device, rate and block size: " + s.device + " " + juce::String (s.sampleRate)
                              + " " + juce::String (s.bufferSize));

                const auto missing = f.host->choose ({ "Nope", "", 0.0, 0, 0 });
                r.expect (missing.message.contains ("Nope") && missing.device == "Simulated Output 2" && missing.chosen == wanted,
                          "a driver type that does not exist is refused, and nothing changes: " + missing.message);

                const auto unplugged = f.host->choose ({ SimulatedAudioDeviceType::typeName, "Simulated Output 9", 0.0, 0, 0 });
                r.expect (unplugged.message.contains ("not connected") && unplugged.device == "Simulated Output 2" && unplugged.chosen == wanted,
                          "a device that is not there is refused, and nothing changes: " + unplugged.message);
                r.expect (SettingsFile (temp.folder).audioChoice() == wanted, "a refused choice is not remembered");

                const auto byType = f.host->choose ({ SimulatedAudioDeviceType::typeName, "", 0.0, 0, 0 });
                r.expect (byType.state == AudioState::open && byType.device == "Simulated Output 1" && byType.bufferSize == 441,
                          "a type with no device chosen opens its default device at its own block size");
            }
        }

        void testSoundAfterSettling (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            Fixture f (temp.folder);
            auto& engine = f.host->getEngine();
            auto& edit = f.host->getEdit();

            bool ran = false;
            f.host->whenSettled ([&] { ran = true; });
            r.expect (! f.host->isSettled() && ! ran, "nothing waiting on the engine runs before it has settled");

            // A tone on one track, through the edit's normal output to the open device.
            engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
            edit.ensureNumberOfAudioTracks (1);
            auto track = te::getAudioTracks (edit)[0];
            auto plugin = edit.getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
            track->pluginList.insertPlugin (plugin, 0, nullptr);
            if (auto* tone = dynamic_cast<te::ToneGeneratorPlugin*> (plugin.get()))
                tone->levelParam->setParameter (0.2f, juce::sendNotificationSync);

            r.expect (f.settle(), "the engine settles within 6 s");
            r.expect (ran, "what was waiting runs once it has");

            bool ranAfter = false;
            f.host->whenSettled ([&] { ranAfter = true; });
            r.expect (ranAfter, "once settled, it runs at once");

            r.expect (f.host->status().settled, "the status says it has settled");

            auto* device = f.openDevice();
            edit.getTransport().play (false);
            pumpFor (200);
            if (device != nullptr)
                device->resetOutputPeak();
            const auto rendered = renderOnAudioThread (device, 200);
            r.expect (rendered == 200, "the audio callback runs block by block: " + juce::String (rendered));
            r.expect (device != nullptr && device->outputPeak() > 0.01f,
                      "sound reaches the device: peak " + juce::String (device != nullptr ? device->outputPeak() : 0.0f));
            // Rendering faster than real time, a block is late only if the machine preempted it.
            r.expect (f.host->lateCallbacks() < 20, "almost no late callbacks rendering faster than real time: "
                                                        + juce::String (f.host->lateCallbacks()) + " of 200");
            edit.getTransport().stop (false, false);
        }

        /** Sleeps through its block when told to, as a slow plugin would. */
        struct SlowCallback final : juce::AudioIODeviceCallback
        {
            void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&) override
            {
                for (int i = 0; i < numOutputs; ++i)
                    if (outputs[i] != nullptr)
                        juce::FloatVectorOperations::clear (outputs[i], numSamples);
                if (slow.load())
                    juce::Thread::sleep (25); // a block of 441 at 44.1 kHz is 10 ms
            }
            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
            std::atomic<bool> slow { false };
        };

        void testLateCallbacks (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            Fixture f (temp.folder);
            SlowCallback slow;
            f.host->getDeviceManager().addAudioCallback (&slow);

            auto* device = f.openDevice();
            renderOnAudioThread (device, 200);
            const auto fast = f.host->lateCallbacks();
            // Not zero on a loaded machine (a CI runner, other tests): a preempted block is late.
            r.expect (fast < 20, "blocks that take less than their length are not late: " + juce::String (fast) + " of 200");

            slow.slow = true;
            renderOnAudioThread (device, 4);
            slow.slow = false;
            const auto late = f.host->lateCallbacks() - fast;
            r.expect (late == 4,
                      "four blocks that took longer than their length count as four late callbacks: " + juce::String (late)
                          + " (the timer sees the whole callback; see EngineHost::CallbackTimer)");
            r.expect (f.host->status().lateCallbacks == f.host->lateCallbacks(), "the status shows them");
            r.expect (f.host->status().deviceDropouts == -1, "a device with no dropout counter says so");

            f.host->getDeviceManager().removeAudioCallback (&slow);
        }

        void testAudioThreads (Results& r)
        {
            const ScopedSilentLog silent;
            const auto cpus = juce::jmax (1, juce::SystemStats::getNumCpus());

            TempFolder temp;
            {
                Fixture f (temp.folder);
                r.expect (f.host->status().audioThreads == 1, "one audio thread by default");
                r.expect (f.host->getEngine().getEngineBehaviour().getNumberOfCPUsToUseForAudio() == 1, "and Tracktion is told so");

                auto c = f.host->status().chosen;
                c.audioThreads = 2;
                const auto s = f.host->choose (c);
                r.expect (s.audioThreads == juce::jmin (2, cpus) && f.host->getEngine().getEngineBehaviour().getNumberOfCPUsToUseForAudio() == s.audioThreads,
                          "the number of audio threads is a setting: " + juce::String (s.audioThreads));

                c.audioThreads = 10000;
                r.expect (f.host->choose (c).audioThreads == cpus, "no more than the machine has");
            }

            r.expect (SettingsFile (temp.folder).audioChoice().audioThreads == 10000, "the chosen number is remembered as chosen");

            Fixture f (temp.folder, SimulatedAudioDeviceType::Clock::manual, SimulatedAudioDeviceType::typeName, 441, 1);
            r.expect (f.host->status().audioThreads == 1, "--audio-threads overrides it");
            r.expect (SettingsFile (temp.folder).audioChoice().audioThreads == 10000, "without changing what is remembered");
        }

        void testExclusiveMode (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            // Stands in for Windows Audio (Exclusive Mode), which opens at a few ms by default.
            Fixture f (temp.folder, SimulatedAudioDeviceType::Clock::manual, "Simulated (Exclusive Mode)", 64);

            auto s = f.host->status();
            r.expect (s.state == AudioState::open && s.bufferSize == 441,
                      "exclusive mode does not open at its default block size, but at 10 ms or more: " + juce::String (s.bufferSize));

            s = f.host->choose ({ "Simulated (Exclusive Mode)", "Simulated Output 2", 44100.0, 128, 0 });
            r.expect (s.bufferSize == 128, "a block size chosen in exclusive mode is kept: " + juce::String (s.bufferSize));

            s = f.host->choose ({ "Simulated (Exclusive Mode)", "Simulated Output 2", 48000.0, 0, 0 });
            r.expect (s.bufferSize == 480, "at 48 kHz, 10 ms is 480 samples: " + juce::String (s.bufferSize));
        }

        void testSameDeviceOtherType (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            // Windows Audio and its exclusive mode list the same devices by the same names.
            const juce::String shared = SimulatedAudioDeviceType::typeName, exclusive = "Simulated (Exclusive Mode)";
            Fixture f (temp.folder, SimulatedAudioDeviceType::Clock::manual, shared, 441, 0, exclusive);

            auto openType = [&f]
            {
                auto* d = f.host->getDeviceManager().getCurrentAudioDevice();
                return d != nullptr ? d->getTypeName() : juce::String ("nothing");
            };

            auto s = f.host->choose ({ shared, "Simulated Output 1", 44100.0, 441, 0 });
            r.expect (s.state == AudioState::open && openType() == shared, "the device opens under the first type: " + openType());

            // Same name, rate and block size: JUCE alone keeps the first type's device open.
            s = f.host->choose ({ exclusive, "Simulated Output 1", 44100.0, 441, 0 });
            r.expect (s.state == AudioState::open && s.type == exclusive && openType() == exclusive,
                      "choosing the same device under another type opens it under that type: " + openType());
            r.expect (f.other->findOpenDevice ("Simulated Output 1") != nullptr && f.type->findOpenDevice ("Simulated Output 1") == nullptr,
                      "and closes the first type's");

            s = f.host->choose ({ shared, "Simulated Output 1", 0.0, 0, 0 });
            r.expect (s.state == AudioState::open && openType() == shared, "and back again: " + openType());
        }

        void testRemovalAndReconnection (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            Fixture f (temp.folder);
            const AudioChoice wanted { SimulatedAudioDeviceType::typeName, "Simulated Output 2", 48000.0, 256, 0 };
            f.host->choose (wanted);
            r.expect (f.settle(), "settled before the device is pulled out");

            int changes = 0;
            f.host->onStatusChanged = [&] (const AudioStatus&) { ++changes; };

            // Pulled out while it is in use.
            renderOnAudioThread (f.openDevice(), 10);
            f.type->unplug ("Simulated Output 2");
            // JUCE closes the device as soon as its driver says the list changed, so it is gone now.
            r.expect (f.type->findOpenDevice ("Simulated Output 2") == nullptr, "an unplugged device is closed");
            r.expect (! f.host->isSettled(), "nothing may start playback on a device that has just changed");

            r.expect (pumpUntil ([&] { return f.host->status().state == AudioState::fallback && changes > 0; }),
                      "the app notices it is gone, falls back and says so: " + toString (f.host->status().state));
            auto s = f.host->status();
            r.expect (s.device == "Simulated Output 1" && s.chosen == wanted, "to another output, keeping the choice: " + s.device);
            r.expect (s.message.contains ("Simulated Output 2"), "and says why: " + s.message);
            r.expect (SettingsFile (temp.folder).audioChoice() == wanted, "the remembered choice is not overwritten by the fallback");
            r.expect (f.type->numOpenDevices() == 1, "only the fallback is open");
            r.expect (renderOnAudioThread (f.openDevice(), 10) == 10, "the fallback plays");
            auto* fallbackDevice = f.openDevice();
            r.expect (fallbackDevice != nullptr && ! fallbackDevice->inputDeviceWasRequested(), "the fallback is opened with outputs only");
            r.expect (! f.host->isSettled(), "playback waits for the engine to settle on the new device");
            r.expect (f.settle(), "and it settles");

            // Plugged in again.
            f.type->plugIn ("Simulated Output 2");
            r.expect (pumpUntil ([&] { return f.host->status().state == AudioState::open; }),
                      "the chosen device is opened again when it comes back");
            s = f.host->status();
            r.expect (s.device == "Simulated Output 2" && juce::exactlyEqual (s.sampleRate, 48000.0) && s.bufferSize == 256,
                      "at the chosen rate and block size: " + s.device + " " + juce::String (s.sampleRate) + " " + juce::String (s.bufferSize));
            r.expect (s.message.isEmpty(), "with nothing to complain about: " + s.message);
            r.expect (renderOnAudioThread (f.openDevice(), 10) == 10, "and it plays");
            r.expect (f.settle(), "and settles again");

            // Every device gone, then one back.
            f.type->unplug ("Simulated Output 2");
            f.type->unplug ("Simulated Output 1");
            r.expect (pumpUntil ([&] { return f.host->status().state == AudioState::none; }), "with no device at all, nothing is open");
            r.expect (f.type->numOpenDevices() == 0, "and no device is left open");
            r.expect (! f.host->isSettled(), "and nothing counts as settled");
            pumpFor (100);

            f.type->plugIn ("Simulated Output 1");
            r.expect (pumpUntil ([&] { return f.host->status().state == AudioState::fallback; }),
                      "a device that appears is used while the chosen one is missing: " + toString (f.host->status().state));
            f.type->plugIn ("Simulated Output 2");
            r.expect (pumpUntil ([&] { return f.host->status().state == AudioState::open; }) && f.host->status().device == "Simulated Output 2",
                      "and the chosen one is back when it is");
            r.expect (f.type->numOpenDevices() == 1, "one device open at the end");
            f.host->onStatusChanged = nullptr;
        }

        /** Devices unplugged and plugged in again while a device's own thread calls back in real time. */
        void testRemovalWhilePlaying (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            Fixture f (temp.folder, SimulatedAudioDeviceType::Clock::realTime);
            f.host->choose ({ SimulatedAudioDeviceType::typeName, "Simulated Output 2", 44100.0, 441, 0 });
            r.expect (f.settle(), "a real-time device settles");

            juce::Random random (27);
            for (int i = 0; i < 25; ++i)
            {
                const juce::String name = random.nextBool() ? "Simulated Output 1" : "Simulated Output 2";
                f.type->unplug (name);
                pumpFor (random.nextInt (40));
                f.type->plugIn (name);
                pumpFor (random.nextInt (40));
            }

            r.expect (pumpUntil ([&] { return f.host->status().state == AudioState::open; }),
                      "after 25 removals while playing, the chosen device is open: " + toString (f.host->status().state));
            r.expect (f.type->numOpenDevices() == 1, "and only it");
            auto* device = f.openDevice();
            const auto blocks = device != nullptr ? device->blocksRendered() : 0;
            pumpFor (200);
            r.expect (device != nullptr && device->blocksRendered() > blocks, "and it is playing");
            r.expect (f.settle(), "and the engine settles");
        }

        /** A device that is still there but has stopped calling back, as a stuck driver does. */
        void testStalledDevice (Results& r)
        {
            const ScopedSilentLog silent;
            TempFolder temp;
            Fixture f (temp.folder, SimulatedAudioDeviceType::Clock::realTime);
            r.expect (f.settle(), "a real-time device settles");

            auto* device = f.openDevice();
            if (device == nullptr)
                return;
            device->freeze (true);

            r.expect (pumpUntil ([&] { return f.host->status().message.contains ("stopped playing"); }, 5000),
                      "a device that stops calling back is noticed: " + f.host->status().message);

            const auto s = f.host->choose (f.host->status().chosen);
            r.expect (pumpUntil ([&] { return f.openDevice() != nullptr && f.openDevice()->blocksRendered() > 10; }, 3000),
                      "choosing it again reopens it, and it plays");
            r.expect (f.host->status().message.isEmpty(), "with nothing to complain about: " + f.host->status().message + " / " + s.message);
        }
    }

    int runAudioTests()
    {
        Results r;
        testSettings (r);
        testOutputsOnly (r);
        testChooseAndRemember (r);
        testSoundAfterSettling (r);
        testLateCallbacks (r);
        testAudioThreads (r);
        testExclusiveMode (r);
        testSameDeviceOtherType (r);
        testRemovalAndReconnection (r);
        testRemovalWhilePlaying (r);
        testStalledDevice (r);

        std::cout << r.passed << " passed, " << r.failed << " failed" << std::endl;
        return r.failed == 0 ? 0 : 1;
    }
}
