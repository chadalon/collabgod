// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineHost.h"
#include "Log.h"

namespace te = tracktion;

namespace collabgod
{
    juce::String toString (AudioState state)
    {
        switch (state)
        {
            case AudioState::open:     return "open";
            case AudioState::fallback: return "fallback";
            case AudioState::none:     return "none";
        }
        return {};
    }

    bool AudioStatus::sameExceptCounters (const AudioStatus& o) const
    {
        return state == o.state && type == o.type && device == o.device
            && juce::exactlyEqual (sampleRate, o.sampleRate) && bufferSize == o.bufferSize
            && juce::exactlyEqual (outputLatencyMs, o.outputLatencyMs)
            && sampleRates == o.sampleRates && bufferSizes == o.bufferSizes
            && chosen == o.chosen && audioThreads == o.audioThreads && cpus == o.cpus
            && settled == o.settled && message == o.message;
    }

    namespace
    {
        constexpr int pollIntervalMs = 20;
        /** A poll counts at most this much towards settling, so a stalled message thread does not
            pass for quiet (webview-ui finding 8). */
        constexpr double maxCountedPollMs = 50.0;
        /** And settling times out only once it has been watched for this long. */
        constexpr double settleObserveMs = 1000.0;

        double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

        juce::String describe (const juce::String& type, const juce::String& device)
        {
            return "\"" + device + "\" (" + type + ")";
        }
    }

    //==============================================================================
    class EngineHost::Behaviour final : public te::EngineBehaviour
    {
    public:
        Behaviour (const std::atomic<int>& t, bool system) : threads (t), systemTypes (system) {}

        /** The app opens the device itself, with outputs only, after adding its own types. */
        bool autoInitialiseDeviceManager() override { return false; }
        bool addSystemAudioIODeviceTypes() override { return systemTypes; }

        /** Read by Tracktion whenever it makes a playback graph, and by
            DeviceManager::updateNumCPUs when this changes. */
        int getNumberOfCPUsToUseForAudio() override { return juce::jmax (1, threads.load()); }

    private:
        const std::atomic<int>& threads;
        const bool systemTypes;
    };

    //==============================================================================
    /** Tracktion's settings, in a folder of their own inside the app's settings folder: its
        file is Settings.xml, which on Windows and macOS (case-insensitive) is the same file as
        the app's settings.xml. The audio device setup is not
        Tracktion's to keep: it comes from the choice in the app's own settings, so there is one
        record of what was chosen, and a fallback device Tracktion saw open is never written
        over it (ADR-0018). */
    class EngineHost::Storage final : public te::PropertyStorage
    {
    public:
        Storage (const juce::File& f, std::function<std::unique_ptr<juce::XmlElement>()> setup)
            : te::PropertyStorage ("CollabGod"), folder (f), deviceSetup (std::move (setup))
        {
        }

        juce::File getAppPrefsFolder() override
        {
            folder.createDirectory();
            return folder;
        }

        std::unique_ptr<juce::XmlElement> getXmlProperty (te::SettingID setting) override
        {
            if (setting == te::SettingID::audio_device_setup)
                return deviceSetup();
            return te::PropertyStorage::getXmlProperty (setting);
        }

        void setXmlProperty (te::SettingID setting, const juce::XmlElement& xml) override
        {
            if (setting != te::SettingID::audio_device_setup)
                te::PropertyStorage::setXmlProperty (setting, xml);
        }

        void removeProperty (te::SettingID setting) override
        {
            if (setting != te::SettingID::audio_device_setup)
                te::PropertyStorage::removeProperty (setting);
        }

    private:
        const juce::File folder;
        std::function<std::unique_ptr<juce::XmlElement>()> deviceSetup;
    };

    //==============================================================================
    /**
        Times every audio callback, so the app counts late ones itself: CoreAudio has no dropout
        counter, and a late callback still makes clean audio, so neither the device nor a check
        of the output can see it (engine-projection finding 13).

        Two callbacks, registered with the device manager before Tracktion's. JUCE calls the
        first one first and the rest from the last to the second, so with [start, end, Tracktion]
        the order is start, Tracktion, end: `end` sees how long Tracktion took. A callback that
        took longer than its block is late. The app's audio test checks the order still holds.

        Audio thread: no locks, no allocation; atomics only.
    */
    class EngineHost::CallbackTimer
    {
    public:
        struct Start final : juce::AudioIODeviceCallback
        {
            explicit Start (CallbackTimer& t) : timer (t) {}

            void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs,
                                                   int numSamples, const juce::AudioIODeviceCallbackContext&) override
            {
                // The first callback writes the device's buffers; the others are added to them.
                for (int i = 0; i < numOutputs; ++i)
                    if (outputs[i] != nullptr)
                        juce::FloatVectorOperations::clear (outputs[i], numSamples);

                timer.startTicks = juce::Time::getHighResolutionTicks();
                timer.samplesThisBlock = numSamples;
            }

            void audioDeviceAboutToStart (juce::AudioIODevice* device) override
            {
                timer.sampleRate.store (device->getCurrentSampleRate());
            }

            void audioDeviceStopped() override {}

            CallbackTimer& timer;
        };

        struct End final : juce::AudioIODeviceCallback
        {
            explicit End (CallbackTimer& t) : timer (t) {}

            void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs,
                                                   int numSamples, const juce::AudioIODeviceCallbackContext&) override
            {
                for (int i = 0; i < numOutputs; ++i)
                    if (outputs[i] != nullptr)
                        juce::FloatVectorOperations::clear (outputs[i], numSamples);

                const auto took = juce::Time::getHighResolutionTicks() - timer.startTicks;
                const auto rate = timer.sampleRate.load();

                if (rate > 0.0 && timer.samplesThisBlock > 0)
                {
                    const auto blockTicks = static_cast<double> (timer.samplesThisBlock) / rate
                                            * static_cast<double> (juce::Time::getHighResolutionTicksPerSecond());
                    if (static_cast<double> (took) > blockTicks)
                        timer.late.fetch_add (1, std::memory_order_relaxed);
                }

                timer.callbacks.fetch_add (1, std::memory_order_relaxed);
            }

            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}

            CallbackTimer& timer;
        };

        CallbackTimer() : start (*this), end (*this) {}

        Start start;
        End end;
        std::atomic<juce::int64> late { 0 }, callbacks { 0 };

    private:
        // Written and read on the audio thread only, between start and end of one callback.
        juce::int64 startTicks = 0;
        int samplesThisBlock = 0;
        std::atomic<double> sampleRate { 0.0 };
    };

    //==============================================================================
    EngineHost::EngineHost (Options options)
        : settings (options.settingsFolder == juce::File() ? SettingsFile::defaultFolder() : options.settingsFolder),
          chosen (settings.audioChoice()),
          audioThreadsOverride (juce::jmax (0, options.audioThreadsOverride)),
          watchForStalls (options.watchForStalls)
    {
        audioThreads.store (threadsFor (chosen));
        timing = std::make_unique<CallbackTimer>();

        engine = std::make_unique<te::Engine> (
            std::make_unique<Storage> (settings.getFolder().getChildFile ("Engine"), [this]
            {
                // What Tracktion opens at start: the remembered choice, outputs only. Nothing
                // remembered: JUCE's default device, which initialise opens with no inputs.
                const auto c = effectiveChoice (chosen);
                return c.device.isEmpty() ? nullptr : c.toDeviceSetupXml();
            }),
            nullptr,
            std::make_unique<Behaviour> (audioThreads, options.systemDevices));

        auto& dm = getDeviceManager();

        for (auto& type : options.extraTypes)
            dm.addAudioDeviceType (std::move (type));
        options.extraTypes.clear();

        // Before Tracktion's own callback: see CallbackTimer.
        dm.addAudioCallback (&timing->start);
        dm.addAudioCallback (&timing->end);

        startSettling();
        // No inputs: v1 does not record, and opening inputs makes macOS ask for the microphone.
        engine->getDeviceManager().initialise (0, 2);

        if (chosen.device.isNotEmpty() && dm.getCurrentAudioDevice() != nullptr
            && dm.getCurrentAudioDevice()->getName() != chosen.device)
            lastMessage = describe (chosen.type, chosen.device) + " did not open";

        dm.addChangeListener (this);
        engine->getDeviceManager().addChangeListener (this);

        edit = te::createEmptyEdit (*engine, juce::File());
        edit->getTransport().addListener (this);
        edit->getTransport().ensureContextAllocated();

        deviceChanged();

        const auto s = status();
        log ("Audio threads: " + juce::String (s.audioThreads) + " of " + juce::String (s.cpus) + " CPUs"
             + (audioThreadsOverride > 0 ? " (--audio-threads)" : ""));

        if (options.pollOnTimer)
            startTimer (pollIntervalMs);
    }

    EngineHost::~EngineHost()
    {
        stopTimer();
        auto& dm = getDeviceManager();
        dm.removeChangeListener (this);
        engine->getDeviceManager().removeChangeListener (this);

        edit->getTransport().removeListener (this);
        edit->getTransport().stop (false, false);
        edit.reset();

        dm.removeAudioCallback (&timing->end);
        dm.removeAudioCallback (&timing->start);
        engine.reset();
        timing.reset();
    }

    juce::AudioDeviceManager& EngineHost::getDeviceManager()
    {
        return engine->getDeviceManager().deviceManager;
    }

    int EngineHost::threadsFor (const AudioChoice& c) const
    {
        const auto cpus = juce::jmax (1, juce::SystemStats::getNumCpus());
        const auto wanted = audioThreadsOverride > 0 ? audioThreadsOverride
                          : c.audioThreads > 0       ? c.audioThreads
                                                     : defaultAudioThreads (cpus);
        return juce::jlimit (1, cpus, wanted);
    }

    /** The choice with its device filled in: a driver type chosen without a device means
        that type's default device. */
    AudioChoice EngineHost::effectiveChoice (AudioChoice c) const
    {
        if (c.type.isNotEmpty() && c.device.isEmpty())
        {
            for (auto* type : engine->getDeviceManager().deviceManager.getAvailableDeviceTypes())
            {
                if (type->getTypeName() == c.type)
                {
                    const auto names = type->getDeviceNames (false);
                    const auto index = type->getDefaultDeviceIndex (false);
                    if (juce::isPositiveAndBelow (index, names.size()))
                        c.device = names[index];
                }
            }
        }

        return c;
    }

    std::vector<AudioDeviceTypeInfo> EngineHost::deviceTypes (bool rescan)
    {
        std::vector<AudioDeviceTypeInfo> types;

        for (auto* type : getDeviceManager().getAvailableDeviceTypes())
        {
            if (rescan)
                type->scanForDevices();
            types.push_back ({ type->getTypeName(), type->getDeviceNames (false) });
        }

        return types;
    }

    juce::String EngineHost::open (const AudioChoice& choice)
    {
        auto& dm = getDeviceManager();
        juce::AudioIODeviceType* type = nullptr;
        juce::StringArray typeNames;

        for (auto* t : dm.getAvailableDeviceTypes())
        {
            typeNames.add (t->getTypeName());
            if (t->getTypeName() == choice.type)
                type = t;
        }

        if (choice.type.isNotEmpty() && type == nullptr)
            return "There is no driver type \"" + choice.type + "\" on this machine. It has: " + typeNames.joinIntoString (", ");

        if (type != nullptr && choice.device.isNotEmpty())
        {
            type->scanForDevices();
            if (! type->getDeviceNames (false).contains (choice.device))
                return describe (choice.type, choice.device) + " is not connected";
        }

        juce::String error;
        reopening = true;

        // The same setup again is not reopened by JUCE; a device that has stopped needs it.
        if (stalled)
        {
            dm.closeAudioDevice();
            stalled = false;
        }

        // Nor is a device of the same name under another driver type: initialiseFromXML
        // changes the type's name but compares only the device names, rate and block size,
        // so the old type's device kept playing. Windows Audio and its exclusive mode list
        // the same devices, and switching between them did nothing (#27, on Chad's desktop).
        if (auto* open = dm.getCurrentAudioDevice(); open != nullptr && choice.type.isNotEmpty() && open->getTypeName() != choice.type)
            dm.closeAudioDevice();

        if (choice.device.isEmpty())
            error = dm.initialise (0, 2, nullptr, false);
        else
            // false: if it will not open, open nothing else; the caller decides what to fall back to.
            error = dm.initialise (0, 2, choice.toDeviceSetupXml().get(), false);

        reopening = false;

        if (error.isEmpty() && dm.getCurrentAudioDevice() == nullptr)
            error = choice.device.isEmpty() ? "There is no audio output device" : describe (choice.type, choice.device) + " did not open";

        return error;
    }

    AudioStatus EngineHost::choose (const AudioChoice& wanted)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        AudioChoice next = wanted;
        next.sampleRate = std::isfinite (next.sampleRate) ? juce::jmax (0.0, next.sampleRate) : 0.0;
        next.bufferSize = juce::jmax (0, next.bufferSize);
        next.audioThreads = juce::jmax (0, next.audioThreads);

        const auto previous = chosen;
        auto deviceOf = [] (AudioChoice c) { c.audioThreads = 0; return c; };
        const bool deviceDiffers = ! (deviceOf (next) == deviceOf (previous)) || status().state != AudioState::open || stalled;

        if (deviceDiffers)
        {
            const auto target = effectiveChoice (next);
            log ("Opening audio output " + describe (target.type.isEmpty() ? "default type" : target.type,
                                                   target.device.isEmpty() ? "default device" : target.device)
                 + (target.sampleRate > 0.0 ? ", " + juce::String (target.sampleRate) + " Hz" : juce::String())
                 + (target.bufferSize > 0 ? ", " + juce::String (target.bufferSize) + " samples" : juce::String()));

            const auto error = open (target);

            if (error.isNotEmpty())
            {
                log ("Could not open it: " + error);
                lastMessage = error;
                // Back to what was open before, so a bad choice does not leave the person in silence.
                if (getDeviceManager().getCurrentAudioDevice() == nullptr)
                    open (effectiveChoice (previous));
                openSomethingIfNone();
                deviceChanged();
                return status();
            }
        }

        chosen = next;
        lastMessage = {};
        lastReconnectAttempt = {};
        settings.setAudioChoice (chosen);

        const auto threads = threadsFor (chosen);
        if (threads != audioThreads.load())
        {
            audioThreads.store (threads);
            engine->getDeviceManager().updateNumCPUs();
            log ("Audio threads: " + juce::String (threads));
        }

        useSafeExclusiveBlockSize();
        deviceChanged();
        return status();
    }

    AudioStatus EngineHost::status() const
    {
        auto& dm = engine->getDeviceManager().deviceManager;
        AudioStatus s;
        s.chosen = chosen;
        s.audioThreads = audioThreads.load();
        s.cpus = juce::jmax (1, juce::SystemStats::getNumCpus());
        s.settled = isSettled();
        s.lateCallbacks = lateCallbacks();
        s.deviceDropouts = dropoutsNow < 0 && dropoutsBefore == 0 ? -1 : dropoutsBefore + juce::jmax ((juce::int64) 0, dropoutsNow);

        auto* device = dm.getCurrentAudioDevice();

        if (device == nullptr)
        {
            s.state = AudioState::none;
            s.message = lastMessage.isNotEmpty() ? lastMessage : juce::String ("No audio output is open");
            return s;
        }

        s.type = device->getTypeName();
        s.device = device->getName();
        s.sampleRate = device->getCurrentSampleRate();
        s.bufferSize = device->getCurrentBufferSizeSamples();
        s.outputLatencyMs = s.sampleRate > 0.0 ? 1000.0 * device->getOutputLatencyInSamples() / s.sampleRate : 0.0;
        s.sampleRates = device->getAvailableSampleRates();
        s.bufferSizes = device->getAvailableBufferSizes();

        const auto want = effectiveChoice (chosen);
        const bool isChosen = want.device.isEmpty() || (s.device == want.device && (want.type.isEmpty() || s.type == want.type));
        s.state = isChosen ? AudioState::open : AudioState::fallback;

        if (s.state == AudioState::fallback)
            s.message = describe (want.type, want.device) + " is not available; playing through "
                      + describe (s.type, s.device) + " until it is back";
        else if (stalled)
            s.message = describe (s.type, s.device) + " has stopped playing. Choose it again, or another output";
        else
            s.message = lastMessage;

        return s;
    }

    juce::int64 EngineHost::lateCallbacks() const noexcept
    {
        return timing->late.load (std::memory_order_relaxed);
    }

    //==============================================================================
    void EngineHost::changeListenerCallback (juce::ChangeBroadcaster* source)
    {
        if (source == &engine->getDeviceManager())
        {
            // Tracktion's device lists (MIDI and wave devices): its first MIDI scan, and every
            // rescan that found a change, rebuild devices and can stop a playing transport (#101).
            ++deviceListChanges;
            lastDeviceChange = nowMs();
            firstScanDone = true;
            return;
        }

        deviceChanged();
    }

    void EngineHost::playbackContextChanged()
    {
        ++contextChanges;
        lastContextChange = nowMs();
    }

    juce::String EngineHost::deviceListKey()
    {
        juce::String key;
        for (auto* type : getDeviceManager().getAvailableDeviceTypes())
            key << type->getTypeName() << ':' << type->getDeviceNames (false).joinIntoString ("|") << ';';
        return key;
    }

    void EngineHost::deviceChanged()
    {
        auto* device = getDeviceManager().getCurrentAudioDevice();
        const auto name = device != nullptr ? device->getName() : juce::String();
        const auto rate = device != nullptr ? device->getCurrentSampleRate() : 0.0;
        const auto block = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;

        if (! deviceKnown || device != lastDevice || name != lastDeviceName || ! juce::exactlyEqual (rate, lastRate) || block != lastBlock)
        {
            deviceKnown = true;
            lastDevice = device;
            lastDeviceName = name;
            lastRate = rate;
            lastBlock = block;

            if (device != nullptr)
                log ("Audio output: " + describe (device->getTypeName(), name) + ", " + juce::String (rate) + " Hz, "
                     + juce::String (block) + " samples per block, outputs only");
            else
                log ("No audio output is open" + (lastMessage.isNotEmpty() ? ": " + lastMessage : juce::String()));

            startSettling();
        }

        updateDropouts();

        if (! reopening)
        {
            reopenChosenIfBack();
            openSomethingIfNone();
        }

        useSafeExclusiveBlockSize();
        notifyIfChanged();
    }

    void EngineHost::reopenChosenIfBack()
    {
        if (chosen.device.isEmpty() && chosen.type.isEmpty())
            return; // nothing chosen: whatever JUCE opens by default is right

        if (status().state == AudioState::open)
        {
            lastReconnectAttempt = {};
            return;
        }

        const auto key = deviceListKey();
        if (key == lastReconnectAttempt)
            return;
        lastReconnectAttempt = key;

        const auto want = effectiveChoice (chosen);
        for (auto* type : getDeviceManager().getAvailableDeviceTypes())
        {
            if (type->getTypeName() != want.type)
                continue;

            if (! type->getDeviceNames (false).contains (want.device))
                return;

            log ("The chosen output " + describe (want.type, want.device) + " is back; opening it");
            const auto error = open (want);
            if (error.isNotEmpty())
            {
                log ("Could not open it: " + error);
                lastMessage = error;
            }
            else
            {
                lastMessage = {};
            }
            return;
        }
    }

    void EngineHost::openSomethingIfNone()
    {
        if (getDeviceManager().getCurrentAudioDevice() != nullptr)
        {
            lastFallbackAttempt = {};
            return;
        }

        const auto key = deviceListKey();
        if (key == lastFallbackAttempt)
            return;
        lastFallbackAttempt = key;

        // The default device of the first driver type that has one, outputs only.
        if (open ({}).isEmpty())
            if (auto* device = getDeviceManager().getCurrentAudioDevice())
                log ("Falling back to " + describe (device->getTypeName(), device->getName()));
    }

    void EngineHost::useSafeExclusiveBlockSize()
    {
        auto& dm = getDeviceManager();
        auto* device = dm.getCurrentAudioDevice();

        if (device == nullptr || ! isExclusiveModeType (device->getTypeName()))
            return;

        // A block size the person chose for this device is theirs to keep.
        const bool chosenHere = status().state == AudioState::open && chosen.bufferSize > 0;
        if (chosenHere)
            return;

        const auto current = device->getCurrentBufferSizeSamples();
        const auto safe = safeExclusiveBlockSize (device->getAvailableBufferSizes(), device->getCurrentSampleRate(), current);

        if (safe <= current)
            return;

        auto setup = dm.getAudioDeviceSetup();
        setup.bufferSize = safe;
        log ("Exclusive mode: " + juce::String (current) + " samples per block dropped out on Windows; using "
             + juce::String (safe) + " (engine-projection finding 12)");
        reopening = true;
        dm.setAudioDeviceSetup (setup, false);
        reopening = false;
    }

    void EngineHost::updateDropouts()
    {
        auto* device = getDeviceManager().getCurrentAudioDevice();

        if (device != dropoutDevice)
        {
            dropoutsBefore += juce::jmax ((juce::int64) 0, dropoutsNow);
            dropoutDevice = device;
        }

        dropoutsNow = device != nullptr ? device->getXRunCount() : -1;
    }

    //==============================================================================
    void EngineHost::startSettling()
    {
        if (settled)
            log ("The audio device restarted; waiting for the engine to settle again");

        settled = false;
        settleStart = nowMs();
        quietMs = -1.0;
        watchedMs = 0.0;
        lastPollMs = -1.0;
    }

    void EngineHost::watchDevice (double now)
    {
        // A driver type that does not announce a device coming back (ASIO, DirectSound) is
        // scanned now and then while the chosen device is missing.
        if (now - lastRescan >= rescanWhileMissingMs && (chosen.type.isNotEmpty() || chosen.device.isNotEmpty())
            && status().state != AudioState::open)
        {
            lastRescan = now;
            for (auto* type : getDeviceManager().getAvailableDeviceTypes())
                if (type->getTypeName() == chosen.type)
                    type->scanForDevices();
            reopenChosenIfBack();
            openSomethingIfNone();
        }

        auto* device = getDeviceManager().getCurrentAudioDevice();
        const auto count = timing->callbacks.load (std::memory_order_relaxed);

        if (device == nullptr || count != lastCallbackCount)
        {
            lastCallbackCount = count;
            lastCallbackSeen = now;
            if (stalled)
            {
                stalled = false;
                if (device != nullptr)
                    log ("The audio device is calling back again");
            }
            return;
        }

        if (watchForStalls && ! stalled && now - lastCallbackSeen >= stallMs)
        {
            stalled = true;
            log (describe (device->getTypeName(), device->getName()) + " has stopped calling back for "
                 + juce::String (juce::roundToInt (now - lastCallbackSeen)) + " ms");
        }
    }

    void EngineHost::poll()
    {
        updateDropouts();

        const double now = nowMs();
        watchDevice (now);

        if (! settled)
        {
            // Quiet means quiet across polls that actually ran: a stalled message thread counts
            // for at most one short poll, so a change message still queued behind the stall is
            // not mistaken for quiet (webview-ui finding 8).
            const double previousPoll = lastPollMs < 0.0 ? settleStart : lastPollMs;
            const double counted = lastPollMs < 0.0 ? 0.0 : juce::jlimit (0.0, maxCountedPollMs, now - lastPollMs);
            lastPollMs = now;

            const bool hasDevice = getDeviceManager().getCurrentAudioDevice() != nullptr;
            const bool ready = hasDevice && edit->getTransport().isPlayContextActive()
                               && (firstScanDone || now - settleStart >= firstDeviceScanMaxMs);
            const bool changed = juce::jmax (lastContextChange, lastDeviceChange) > previousPoll;

            if (! ready || changed)
                quietMs = ready ? 0.0 : -1.0;
            else
                quietMs = quietMs < 0.0 ? 0.0 : quietMs + counted;

            watchedMs += counted;

            const bool quiet = ready && quietMs >= settleQuietMs;
            const bool tooLong = hasDevice && now - settleStart >= settleMaxMs && watchedMs >= settleObserveMs;

            if (quiet || tooLong)
            {
                settled = true;
                log ("Engine settled after " + juce::String (juce::roundToInt (now - settleStart)) + " ms, " + juce::String (contextChanges)
                     + " playback context changes and " + juce::String (deviceListChanges) + " device list changes so far"
                     + (quiet ? "" : " (timed out waiting for quiet)"));

                auto toRun = std::move (waiting);
                waiting.clear();
                for (auto& fn : toRun)
                    fn();
            }
        }

        notifyIfChanged();
    }

    bool EngineHost::isSettled() const
    {
        auto* device = engine->getDeviceManager().deviceManager.getCurrentAudioDevice();
        // The pointer alone is not enough: a new device can be made where the old one was.
        return settled && device != nullptr && device == lastDevice && device->getName() == lastDeviceName
            && juce::exactlyEqual (device->getCurrentSampleRate(), lastRate) && device->getCurrentBufferSizeSamples() == lastBlock;
    }

    void EngineHost::whenSettled (std::function<void()> fn)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (isSettled())
            fn();
        else
            waiting.push_back (std::move (fn));
    }

    void EngineHost::notifyIfChanged()
    {
        const auto s = status();

        if (lastNotified.has_value() && s.sameExceptCounters (*lastNotified))
            return;

        lastNotified = s;
        if (onStatusChanged != nullptr)
            onStatusChanged (s);
    }
}
