// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "AudioSettings.h"

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace collabgod
{
    /** Which output is open, against what was chosen. */
    enum class AudioState
    {
        /** The chosen output is open (the default output when nothing was chosen). */
        open,
        /** The chosen output is not available, and another one is open in its place. */
        fallback,
        /** No output is open: there is no device, or none would open. */
        none
    };

    /** A driver type and the output devices it has now. */
    struct AudioDeviceTypeInfo
    {
        juce::String name;
        juce::StringArray devices;
    };

    /** What the person needs to know about audio output. Message thread. */
    struct AudioStatus
    {
        AudioState state = AudioState::none;
        juce::String type, device;
        double sampleRate = 0.0;
        int bufferSize = 0;
        double outputLatencyMs = 0.0;
        juce::Array<double> sampleRates;
        juce::Array<int> bufferSizes;
        /** What was chosen and is remembered; empty fields are defaults. */
        AudioChoice chosen;
        /** CPUs the engine uses for audio now, the audio thread included. */
        int audioThreads = 1;
        int cpus = 1;
        /** The engine has settled since the device last started: safe to start playback. */
        bool settled = false;
        /** Audio callbacks that took longer than their block, timed by the app itself. */
        juce::int64 lateCallbacks = 0;
        /** Dropouts the driver reports, summed over every device opened; -1 if the device does
            not count them (CoreAudio never does). */
        juce::int64 deviceDropouts = -1;
        /** Why the last choice did not open as chosen, or why there is no output. Empty if it did. */
        juce::String message;

        /** The same except for the counters, which change all the time. */
        bool sameExceptCounters (const AudioStatus&) const;
    };

    /**
        The app's audio engine: Tracktion Engine, the audio output, and one edit for it to play
        (#27). Owns the choice of output device and keeps it working:

        - Opens outputs only, never inputs, so macOS does not ask for the microphone.
        - Remembers what the person chose in the settings file, and opens it at start.
        - When the chosen device goes away it falls back to another output, keeps the choice,
          and opens the chosen device again when it comes back.
        - Exclusive mode on Windows is never the default, and opens at a block size of at
          least 10 ms unless one was chosen (engine-projection finding 12).
        - The number of audio threads is its setting (engine-projection findings 5 and 13).
        - Counts late audio callbacks itself (engine-projection finding 13).
        - Says when the engine has settled after the device starts, and holds back anything
          that would start playback until then (engine-toolchain and webview-ui finding 8).

        Message thread only, except where a function says otherwise.
    */
    class EngineHost final : private juce::ChangeListener,
                             private tracktion::TransportControl::Listener,
                             private juce::Timer
    {
    public:
        struct Options
        {
            /** Where settings are kept; empty for SettingsFile::defaultFolder(). */
            juce::File settingsFolder;
            /** Offer the operating system's driver types (WASAPI, ASIO, CoreAudio and the rest). */
            bool systemDevices = true;
            /** A driver type of the test's or of --simulated-audio's, offered alongside (or instead). */
            std::unique_ptr<juce::AudioIODeviceType> extraType;
            /** --audio-threads: use this many, without remembering it; 0 for the remembered one. */
            int audioThreadsOverride = 0;
            /** Run the settle check on a timer. Tests turn it off and call poll() themselves. */
            bool pollOnTimer = true;
            /** Say so when an open device stops calling back. Off for a test's manual device,
                which calls back only when the test asks. */
            bool watchForStalls = true;
        };

        explicit EngineHost (Options);
        ~EngineHost() override;

        /** The driver types and their output devices, scanned again if `rescan`. */
        std::vector<AudioDeviceTypeInfo> deviceTypes (bool rescan);

        /** Opens this output and remembers it. If it will not open, the previous output (or
            another one) stays open and the status says why. Returns the status after. */
        AudioStatus choose (const AudioChoice&);

        AudioStatus status() const;

        /** Late callbacks so far, timed by the app. Any thread. */
        juce::int64 lateCallbacks() const noexcept;

        /** Whether the engine has settled since the device last started. False from the moment
            the device changes, before the change message that restarts settling arrives. */
        bool isSettled() const;

        /** Runs `fn` once the engine has settled: now if it has, otherwise later on the
            message thread. Anything that starts playback goes through here (#30). Calls
            waiting when the device restarts wait for it to settle again. */
        void whenSettled (std::function<void()> fn);

        /** Called on the message thread whenever the status changes, other than its counters. */
        std::function<void (const AudioStatus&)> onStatusChanged;

        /** The settle check and device bookkeeping; the timer calls it every 20 ms. */
        void poll();

        tracktion::Engine& getEngine() { return *engine; }
        tracktion::Edit& getEdit() { return *edit; }
        juce::AudioDeviceManager& getDeviceManager();
        SettingsFile& getSettings() { return settings; }

        /** The block size it uses in exclusive mode when none was chosen. See AudioSettings.h. */
        static constexpr double exclusiveMinimumBlockMs = 10.0;

        /** How long settling waits for the first MIDI device scan, quiet after the last change,
            and in all (webview-ui finding 8). */
        static constexpr double firstDeviceScanMaxMs = 1500.0;
        static constexpr double settleQuietMs = 300.0;
        static constexpr double settleMaxMs = 3000.0;

        /** While the chosen device is missing, its driver type's devices are scanned this often:
            ASIO and DirectSound do not say when a device comes back. */
        static constexpr double rescanWhileMissingMs = 2000.0;
        /** An open device that has not called back for this long has stalled. */
        static constexpr double stallMs = 2000.0;

    private:
        class Behaviour;
        class Storage;
        class CallbackTimer;

        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        void playbackContextChanged() override;
        void timerCallback() override { poll(); }

        AudioChoice effectiveChoice (AudioChoice) const;
        juce::String open (const AudioChoice&);
        void deviceChanged();
        void reopenChosenIfBack();
        void openSomethingIfNone();
        void useSafeExclusiveBlockSize();
        void startSettling();
        void updateDropouts();
        void watchDevice (double now);
        void notifyIfChanged();
        int threadsFor (const AudioChoice&) const;
        juce::String deviceListKey();

        SettingsFile settings;
        AudioChoice chosen;
        const int audioThreadsOverride;
        std::atomic<int> audioThreads { 1 };
        juce::String lastMessage;

        std::unique_ptr<CallbackTimer> timing;
        std::unique_ptr<tracktion::Engine> engine;
        std::unique_ptr<tracktion::Edit> edit;

        // What was open at the last device change, to notice the device restarting.
        bool deviceKnown = false;
        juce::AudioIODevice* lastDevice = nullptr;
        juce::String lastDeviceName;
        double lastRate = 0.0;
        int lastBlock = 0;

        // Reconnecting: the device lists the last attempt saw, so a device that will not open
        // is not tried again until something is plugged in or out.
        juce::String lastReconnectAttempt, lastFallbackAttempt;
        bool reopening = false;

        // Rescanning while the chosen device is missing, and noticing a device that stopped.
        const bool watchForStalls;
        double lastRescan = 0.0, lastCallbackSeen = 0.0;
        juce::int64 lastCallbackCount = -1;
        bool stalled = false;

        // Dropouts the drivers report, summed over devices.
        juce::int64 dropoutsBefore = 0, dropoutsNow = -1;
        juce::AudioIODevice* dropoutDevice = nullptr;

        // Settling (engine-toolchain finding 8, webview-ui finding 8)
        bool settled = false;
        bool firstScanDone = false;
        double settleStart = 0.0, lastContextChange = 0.0, lastDeviceChange = 0.0;
        double quietMs = -1.0, watchedMs = 0.0, lastPollMs = -1.0;
        int contextChanges = 0, deviceListChanges = 0;
        std::vector<std::function<void()>> waiting;

        std::optional<AudioStatus> lastNotified;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineHost)
    };

    juce::String toString (AudioState);
}
