// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SimulatedAudio.h"

namespace collabgod
{
    //==============================================================================
    /** With Clock::realTime: calls the device's audio callback one block at a time, paced to
        real time, as a driver's audio thread would. */
    class SimulatedAudioDevice::RealTimeClock final : public juce::Thread
    {
    public:
        explicit RealTimeClock (SimulatedAudioDevice& d) : juce::Thread ("Simulated sound card"), device (d) {}
        ~RealTimeClock() override { stopThread (2000); }

        void run() override
        {
            const double blockMs = 1000.0 * device.blockSize / device.sampleRate;
            double next = juce::Time::getMillisecondCounterHiRes();

            while (! threadShouldExit())
            {
                if (! device.renderOne())
                {
                    wait (5);
                    next = juce::Time::getMillisecondCounterHiRes();
                    continue;
                }

                next += blockMs;
                const double ahead = next - juce::Time::getMillisecondCounterHiRes();

                if (ahead >= 1.0)
                    wait (static_cast<int> (ahead));
                else if (ahead < -100.0)
                    next = juce::Time::getMillisecondCounterHiRes(); // far behind: do not try to catch up
            }
        }

    private:
        SimulatedAudioDevice& device;
    };

    //==============================================================================
    SimulatedAudioDeviceType::SimulatedAudioDeviceType (Clock c, juce::StringArray deviceNames, const juce::String& name, int block)
        : juce::AudioIODeviceType (name), clock (c), defaultBlock (block), present (std::move (deviceNames))
    {
    }

    SimulatedAudioDeviceType::~SimulatedAudioDeviceType()
    {
        // The device manager deletes its devices before their type.
        jassert (open.isEmpty());
    }

    juce::StringArray SimulatedAudioDeviceType::getDeviceNames (bool) const
    {
        return present;
    }

    int SimulatedAudioDeviceType::getDefaultDeviceIndex (bool) const
    {
        return 0;
    }

    int SimulatedAudioDeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const
    {
        if (device == nullptr)
            return -1;

        const auto* simulated = dynamic_cast<SimulatedAudioDevice*> (device);
        if (asInput && simulated != nullptr && ! simulated->inputDeviceWasRequested())
            return -1;

        return present.indexOf (device->getName());
    }

    juce::AudioIODevice* SimulatedAudioDeviceType::createDevice (const juce::String& outputDeviceName,
                                                                  const juce::String& inputDeviceName)
    {
        const auto name = outputDeviceName.isNotEmpty() ? outputDeviceName : inputDeviceName;

        if (! present.contains (name))
            return nullptr;

        auto* device = new SimulatedAudioDevice (*this, name, inputDeviceName);
        const juce::ScopedLock sl (openLock);
        open.add (device);
        return device;
    }

    void SimulatedAudioDeviceType::deviceDeleted (SimulatedAudioDevice* device)
    {
        const juce::ScopedLock sl (openLock);
        open.removeFirstMatchingValue (device);
    }

    void SimulatedAudioDeviceType::unplug (const juce::String& name)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        present.removeString (name);

        juce::Array<SimulatedAudioDevice*> affected;
        {
            const juce::ScopedLock sl (openLock);
            for (auto* device : open)
                if (device->getName() == name)
                    affected.add (device);
        }

        for (auto* device : affected)
            device->unplugged();

        callDeviceChangeListeners();
    }

    void SimulatedAudioDeviceType::plugIn (const juce::String& name)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        present.addIfNotAlreadyThere (name);
        callDeviceChangeListeners();
    }

    void SimulatedAudioDeviceType::unplugListed (const juce::String& name)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        goneButListed.addIfNotAlreadyThere (name);

        const juce::ScopedLock sl (openLock);
        for (auto* device : open)
            if (device->getName() == name)
                device->freeze (true);
    }

    void SimulatedAudioDeviceType::plugInListed (const juce::String& name)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        goneButListed.removeString (name);
    }

    bool SimulatedAudioDeviceType::isGoneButListed (const juce::String& name) const
    {
        return goneButListed.contains (name);
    }

    SimulatedAudioDevice* SimulatedAudioDeviceType::findOpenDevice (const juce::String& name) const
    {
        const juce::ScopedLock sl (openLock);
        for (auto* device : open)
            if (device->getName() == name && device->isOpen())
                return device;
        return nullptr;
    }

    int SimulatedAudioDeviceType::numOpenDevices() const
    {
        const juce::ScopedLock sl (openLock);
        int n = 0;
        for (auto* device : open)
            if (device->isOpen())
                ++n;
        return n;
    }

    //==============================================================================
    SimulatedAudioDevice::SimulatedAudioDevice (SimulatedAudioDeviceType& t, const juce::String& deviceName, const juce::String& inputName)
        : juce::AudioIODevice (deviceName, t.getTypeName()),
          type (t),
          inputRequested (inputName.isNotEmpty()),
          blockSize (t.getDefaultBlockSize())
    {
    }

    SimulatedAudioDevice::~SimulatedAudioDevice()
    {
        close();
        type.deviceDeleted (this);
    }

    juce::String SimulatedAudioDevice::open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                                             double rate, int size)
    {
        close();

        if (removed)
            return lastError = "The device has been unplugged";
        if (type.isGoneButListed (getName()))
            return lastError = "Cannot open " + getName() + " (the hardware is not there)";

        const auto rates = getAvailableSampleRates();
        const auto sizes = getAvailableBufferSizes();
        sampleRate = rates.contains (rate) ? rate : defaultSampleRate;
        blockSize = sizes.contains (size) && ! type.opensAtDefaultBlockSize() ? size : type.getDefaultBlockSize();

        // Inputs open only when an input device was asked for, as with a real driver.
        inputs = inputRequested ? inputChannels : juce::BigInteger();
        inputs.setRange (2, juce::jmax (0, inputs.getHighestBit() - 1), false);
        outputs = outputChannels;
        outputs.setRange (2, juce::jmax (0, outputs.getHighestBit() - 1), false);

        buffer.setSize (2, blockSize);
        if (type.getClock() == SimulatedAudioDeviceType::Clock::realTime)
            clock = std::make_unique<RealTimeClock> (*this);

        opened = true;
        lastError = {};
        return {};
    }

    void SimulatedAudioDevice::close()
    {
        stop();
        clock.reset();
        opened = false;
    }

    void SimulatedAudioDevice::start (juce::AudioIODeviceCallback* newCallback)
    {
        if (! opened || newCallback == nullptr)
            return;

        stop();
        newCallback->audioDeviceAboutToStart (this);

        {
            const juce::ScopedLock sl (callbackLock);
            callback = newCallback;
        }

        if (clock != nullptr)
            clock->startThread (juce::Thread::Priority::highest);
    }

    void SimulatedAudioDevice::stop()
    {
        juce::AudioIODeviceCallback* old = nullptr;
        {
            const juce::ScopedLock sl (callbackLock);
            std::swap (old, callback);
        }

        if (clock != nullptr)
            clock->stopThread (2000);

        if (old != nullptr)
            old->audioDeviceStopped();
    }

    bool SimulatedAudioDevice::isPlaying()
    {
        const juce::ScopedLock sl (callbackLock);
        return callback != nullptr && ! removed;
    }

    bool SimulatedAudioDevice::renderOne()
    {
        const juce::ScopedLock sl (callbackLock);

        if (callback == nullptr || removed || frozen.load())
            return false;

        buffer.clear();
        const float* inputPointers[2] = { buffer.getReadPointer (0), buffer.getReadPointer (1) };
        const auto numInputs = inputs.countNumberOfSetBits();
        callback->audioDeviceIOCallbackWithContext (inputPointers, numInputs, buffer.getArrayOfWritePointers(), 2,
                                                    blockSize, juce::AudioIODeviceCallbackContext {});

        const auto loudest = juce::jmax (buffer.getMagnitude (0, 0, blockSize), buffer.getMagnitude (1, 0, blockSize));
        if (loudest > peak.load())
            peak.store (loudest);

        blocks.fetch_add (1);
        return true;
    }

    int SimulatedAudioDevice::render (int numBlocks)
    {
        jassert (type.getClock() == SimulatedAudioDeviceType::Clock::manual);

        int rendered = 0;
        for (int i = 0; i < numBlocks && renderOne(); ++i)
            ++rendered;
        return rendered;
    }

    void SimulatedAudioDevice::unplugged()
    {
        juce::AudioIODeviceCallback* current = nullptr;
        {
            const juce::ScopedLock sl (callbackLock);
            removed = true;
            current = callback;
        }

        lastError = "The device was unplugged";

        // As a driver does when its device goes away under it.
        if (current != nullptr)
            current->audioDeviceError (lastError);
    }
}
