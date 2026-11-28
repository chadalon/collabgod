// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <atomic>

namespace collabgod
{
    class SimulatedAudioDevice;

    /**
        A driver type whose devices are simulated sound cards, for tests and for running the app
        where there is no sound card (CI runners have none). Its devices can be unplugged and
        plugged in again, which is what a real USB interface does and what Tracktion's
        HostedAudioDeviceInterface, with its one device that is always there, cannot do.

        It goes through the real juce::AudioDeviceManager and Tracktion's DeviceManager like any
        driver, so a test of choosing, remembering, removing and reconnecting a device runs the
        app's own code on the same path a real device takes.

        Every device offers inputs as well as outputs, so a test can check the app opens outputs
        only (macOS asks for the microphone when inputs are opened).

        The device list and unplugging are message-thread only.
    */
    class SimulatedAudioDeviceType final : public juce::AudioIODeviceType
    {
    public:
        static constexpr const char* typeName = "Simulated";

        enum class Clock
        {
            /** The test calls SimulatedAudioDevice::render, block by block, on its own thread. */
            manual,
            /** Each open device has a thread that calls the audio callback in real time. */
            realTime
        };

        /** `name` is the driver type's name: one with "Exclusive" in it stands in for Windows
            Audio's exclusive mode. `defaultBlockSize` is what its devices open at unasked. */
        explicit SimulatedAudioDeviceType (Clock,
                                           juce::StringArray deviceNames = { "Simulated Output 1", "Simulated Output 2" },
                                           const juce::String& name = typeName,
                                           int defaultBlockSize = 441);
        ~SimulatedAudioDeviceType() override;

        // juce::AudioIODeviceType
        void scanForDevices() override {}
        juce::StringArray getDeviceNames (bool wantInputNames = false) const override;
        int getDefaultDeviceIndex (bool forInput) const override;
        int getIndexOfDevice (juce::AudioIODevice*, bool asInput) const override;
        bool hasSeparateInputsAndOutputs() const override { return true; }
        juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override;

        /** The device is pulled out: it stops calling back, reports an error the way a driver
            does, and leaves the device list, which tells the device manager. */
        void unplug (const juce::String& name);

        /** The device is back in the list, and the device manager is told. */
        void plugIn (const juce::String& name);

        /** Pulled out the way an ASIO driver sees it: the device stays listed and nobody is
            told, but an open one stops calling back and it will not open again until
            `plugInListed`, which tells nobody either. */
        void unplugListed (const juce::String& name);
        void plugInListed (const juce::String& name);
        bool isGoneButListed (const juce::String& name) const;

        /** The open device with this name, or nullptr. */
        SimulatedAudioDevice* findOpenDevice (const juce::String& name) const;

        /** How many devices of this type are open now; one at most when the app is right. */
        int numOpenDevices() const;

        Clock getClock() const noexcept { return clock; }
        int getDefaultBlockSize() const noexcept { return defaultBlock; }

        /** Devices that list every block size but open at their default whatever is asked, as
            some real drivers do. Set before the device is opened. */
        void setOpensAtDefaultBlockSize (bool shouldIgnore) noexcept { ignoresBlockSize = shouldIgnore; }
        bool opensAtDefaultBlockSize() const noexcept { return ignoresBlockSize; }

    private:
        friend class SimulatedAudioDevice;
        void deviceDeleted (SimulatedAudioDevice*);

        const Clock clock;
        const int defaultBlock;
        bool ignoresBlockSize = false;
        juce::StringArray present, goneButListed;
        juce::Array<SimulatedAudioDevice*> open;
        mutable juce::CriticalSection openLock;

        JUCE_DECLARE_NON_COPYABLE (SimulatedAudioDeviceType)
    };

    /** One simulated sound card. Two outputs and two inputs, a few standard rates and sizes. */
    class SimulatedAudioDevice final : public juce::AudioIODevice
    {
    public:
        SimulatedAudioDevice (SimulatedAudioDeviceType&, const juce::String& deviceName, const juce::String& inputName);
        ~SimulatedAudioDevice() override;

        static constexpr double defaultSampleRate = 44100.0;

        // juce::AudioIODevice
        juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
        juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
        juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 96000.0 }; }
        juce::Array<int> getAvailableBufferSizes() override { return { 64, 128, 256, 441, 480, 512, 1024 }; }
        int getDefaultBufferSize() override { return type.getDefaultBlockSize(); }
        juce::String open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                           double sampleRate, int bufferSizeSamples) override;
        void close() override;
        bool isOpen() override { return opened; }
        void start (juce::AudioIODeviceCallback*) override;
        void stop() override;
        bool isPlaying() override;
        juce::String getLastError() override { return lastError; }
        int getCurrentBufferSizeSamples() override { return blockSize; }
        double getCurrentSampleRate() override { return sampleRate; }
        int getCurrentBitDepth() override { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return outputs; }
        juce::BigInteger getActiveInputChannels() const override { return inputs; }
        int getOutputLatencyInSamples() override { return blockSize; }
        int getInputLatencyInSamples() override { return 0; }

        /** Calls the audio callback for this many blocks on the calling thread, which plays the
            part of the driver's audio thread. Returns how many were rendered: none once the
            device is stopped or unplugged. Only with Clock::manual. */
        int render (int blocks);

        /** A frozen device stays connected and open but stops calling back, as a driver that has
            got stuck does. */
        void freeze (bool shouldFreeze) noexcept { frozen.store (shouldFreeze); }

        /** What the driver says it has dropped; -1, the default, for a driver that does not count. */
        void setDropouts (int n) noexcept { dropouts.store (n); }
        int getXRunCount() const noexcept override { return dropouts.load(); }

        /** Whether an input device was asked for when this device was made. */
        bool inputDeviceWasRequested() const noexcept { return inputRequested; }

        /** The loudest sample written to either output, since this was last reset. */
        float outputPeak() const noexcept { return peak.load(); }
        void resetOutputPeak() noexcept { peak.store (0.0f); }

        /** Blocks the audio callback was called for. */
        juce::int64 blocksRendered() const noexcept { return blocks.load(); }

    private:
        friend class SimulatedAudioDeviceType;
        class RealTimeClock;

        /** What the driver's audio thread does for one block. Holds the callback lock. */
        bool renderOne();
        void unplugged();

        SimulatedAudioDeviceType& type;
        const bool inputRequested;
        juce::BigInteger inputs, outputs;
        double sampleRate = defaultSampleRate;
        int blockSize = 0;
        bool opened = false;
        bool removed = false;
        juce::String lastError;

        // The audio callback and whether it runs: written by start, stop and unplugging on the
        // message thread, read by whichever thread renders. A driver holds a lock like this too.
        juce::CriticalSection callbackLock;
        juce::AudioIODeviceCallback* callback = nullptr;

        juce::AudioBuffer<float> buffer;
        std::atomic<float> peak { 0.0f };
        std::atomic<juce::int64> blocks { 0 };
        std::atomic<bool> frozen { false };
        std::atomic<int> dropouts { -1 };
        std::unique_ptr<RealTimeClock> clock;

        JUCE_DECLARE_NON_COPYABLE (SimulatedAudioDevice)
    };
}
