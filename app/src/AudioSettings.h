// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace collabgod
{
    /**
        The audio output a person chose (#27). An empty field means "the default": the first
        driver type with a device, that type's default device, the device's own sample rate and
        block size, and the app's default number of audio threads.

        This is what is remembered, not what is open. When the chosen device is unplugged the
        app falls back to another one, and this stays as it was, so the chosen device is opened
        again when it comes back (ADR-0018).
    */
    struct AudioChoice
    {
        /** A JUCE device type name, such as "Windows Audio", "ASIO" or "CoreAudio". */
        juce::String type;
        /** The output device's name, as its driver type lists it. */
        juce::String device;
        /** Samples per second; 0 for the device's own. */
        double sampleRate = 0.0;
        /** Samples per block; 0 for the device's own (or, in exclusive mode, a safe one). */
        int bufferSize = 0;
        /** CPUs Tracktion may use for audio, the audio thread included (1 means no worker
            threads); 0 for defaultAudioThreads(). */
        int audioThreads = 0;

        bool operator== (const AudioChoice& o) const
        {
            return type == o.type && device == o.device && juce::exactlyEqual (sampleRate, o.sampleRate)
                && bufferSize == o.bufferSize && audioThreads == o.audioThreads;
        }

        std::unique_ptr<juce::XmlElement> toXml() const;
        static AudioChoice fromXml (const juce::XmlElement*);

        /** The JUCE AudioDeviceManager state that opens this choice with outputs only: no input
            device and no input channels, so macOS never asks for the microphone. */
        std::unique_ptr<juce::XmlElement> toDeviceSetupXml() const;
    };

    /**
        How many CPUs the engine uses for audio when nobody has chosen. One: the audio thread
        alone, no worker pool. On a 4-core Intel Mac Tracktion's default (every CPU) made audio
        callbacks late during edits and one thread did not (engine-projection findings 5 and 13);
        a 16-core desktop was clean either way. One thread is not yet shown to be enough under
        plugin load. #113 decides the default; until then this is the safe choice everywhere.
    */
    int defaultAudioThreads (int numCpus);

    /** Whether a device type opens devices in exclusive mode, which on Chad's Windows machine
        dropped out constantly at its default block size (engine-projection finding 12). */
    bool isExclusiveModeType (const juce::String& type);

    /** The block size to use in exclusive mode when nobody chose one: the smallest the device
        offers that is at least 10 ms long, the length shared mode used without a problem
        (441 samples at 44.1 kHz). Falls back to the largest it offers, or `current`. */
    int safeExclusiveBlockSize (const juce::Array<int>& available, double sampleRate, int current);

    /**
        The app's own settings file, `settings.xml` in a folder of its own (the app's data
        folder, or a temporary one in tests). Saved as soon as something changes.
        Message thread only.
    */
    class SettingsFile final
    {
    public:
        explicit SettingsFile (const juce::File& folder);

        /** The CollabGod folder in the system's application data folder. */
        static juce::File defaultFolder();

        juce::File getFolder() const { return folder; }
        juce::File getFile() const;

        AudioChoice audioChoice() const;
        void setAudioChoice (const AudioChoice&);

    private:
        const juce::File folder;
        std::unique_ptr<juce::PropertiesFile> properties;

        JUCE_DECLARE_NON_COPYABLE (SettingsFile)
    };
}
