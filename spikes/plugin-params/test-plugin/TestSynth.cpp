// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.
//
// A VST3 instrument that plays a steady tone, so the self-tests can check that a
// mirrored change changes the sound without any MIDI. Built twice: version 1
// and version 2, which adds a parameter at the front of the list, so every
// shared parameter has a different index but the same ID. The sync must survive
// that (it matches parameters by ID, never by index). Version 2 also saves its
// state in a newer format, which version 1 refuses to load, as a plugin may.
//
// It plays one cycle of a wavetable, which is part of its state but not a
// parameter: like a wavetable, an LFO shape or a loaded sample in a real plugin
// (issue #13). Controls that are not automatable let a host make the plugin
// behave like a real one would, without a window or a mouse. Not being
// automatable, Tracktion Engine skips them and they are never synced:
// - "Test: move pitch" makes the plugin sweep its own Pitch over one second,
//   with a gesture, exactly as dragging a knob in its window would report it.
// - "Test: echo host changes" makes it report every value the host sets back,
//   rounded to two decimals, as some plugins do.
// - "Test: new wavetable" draws a new wavetable: a change to the hidden part.
// - "Test: unreported preset" draws a new wavetable and moves Level and Pitch
//   without telling the host, as a preset load does in some plugins.
// - "Test: embedded data" puts 0, 64 KiB, 1 MiB or 8 MiB of random bytes in the
//   state, as a plugin that embeds a sample does.
// - "Test: use a sample file" writes a small file next to the system's temporary
//   folder and refers to it by its path in the state, as a sampler does.
// - "Test: call setDirty" (on by default) makes it tell the host when the
//   hidden part changes. Off, it says nothing, as some plugins do.
// - "Test: noisy state" puts a counter in the state that changes on every save.
// - "Test: settle late" makes it rewrite its state 300 ms after loading one,
//   as a plugin that finishes loading in the background does.
// - "Test: sample missing" is read-only: 1 when the state refers to a file that
//   is not on this machine.

#include <JuceHeader.h>

#ifndef TEST_SYNTH_VERSION
 #define TEST_SYNTH_VERSION 1
#endif

namespace
{
constexpr double minHz = 110.0;
constexpr double maxHz = 880.0;
constexpr int sweepTicks = 200;      // one tick every 5 ms: one second
constexpr int timerMs = 5;
constexpr int tableSize = 2048;
constexpr juce::uint32 stateMagic = 0x43475453;   // "CGTS"
constexpr int stateFormat = TEST_SYNTH_VERSION;   // version 2 saves format 2, which version 1 refuses

std::vector<float> makeTable (juce::int64 seed)
{
    std::vector<float> t ((size_t) tableSize);

    if (seed == 0)
    {
        for (int i = 0; i < tableSize; ++i)
            t[(size_t) i] = (float) std::sin (juce::MathConstants<double>::twoPi * i / tableSize);
        return t;
    }

    // A few harmonics with amplitudes drawn from the seed, normalised.
    juce::Random r (seed);
    float amps[8];
    for (auto& a : amps)
        a = r.nextFloat();
    amps[0] = 1.0f;

    float peak = 0.0f;
    for (int i = 0; i < tableSize; ++i)
    {
        double s = 0.0;
        for (int h = 0; h < 8; ++h)
            s += amps[h] * std::sin (juce::MathConstants<double>::twoPi * (h + 1) * i / tableSize);
        t[(size_t) i] = (float) s;
        peak = std::max (peak, std::abs ((float) s));
    }

    for (auto& v : t)
        v /= peak;

    return t;
}
} // namespace

class TestSynth final : public juce::AudioProcessor,
                        private juce::Timer
{
public:
    TestSynth()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        auto testBool = [this] (const char* id, const char* name, bool initial = false)
        {
            auto* p = new juce::AudioParameterBool (juce::ParameterID { id, 1 }, name, initial,
                                                    juce::AudioParameterBoolAttributes().withAutomatable (false));
            addParameter (p);
            return p;
        };

       #if TEST_SYNTH_VERSION >= 2
        addParameter (drive = new juce::AudioParameterFloat (juce::ParameterID { "drive", 2 }, "Drive",
                                                             juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f));
       #endif
        addParameter (level = new juce::AudioParameterFloat (juce::ParameterID { "level", 1 }, "Level",
                                                             juce::NormalisableRange<float> (0.0f, 1.0f), 0.25f));
        addParameter (pitch = new juce::AudioParameterFloat (juce::ParameterID { "pitch", 1 }, "Pitch",
                                                             juce::NormalisableRange<float> (0.0f, 1.0f), 0.25f));
        addParameter (wave = new juce::AudioParameterChoice (juce::ParameterID { "wave", 1 }, "Wave",
                                                             juce::StringArray { "Sine", "Square" }, 0));
        testMove = testBool ("testMove", "Test: move pitch");
        testEcho = testBool ("testEcho", "Test: echo host changes");
        testTable = testBool ("testTable", "Test: new wavetable");
        testPreset = testBool ("testPreset", "Test: unreported preset");
        testSample = testBool ("testSample", "Test: use a sample file");
        testDirty = testBool ("testDirty", "Test: call setDirty", true);
        testNoisy = testBool ("testNoisy", "Test: noisy state");
        testSettle = testBool ("testSettle", "Test: settle late");
        addParameter (testData = new juce::AudioParameterChoice (juce::ParameterID { "testData", 1 }, "Test: embedded data",
                                                                 juce::StringArray { "None", "64 KiB", "1 MiB", "8 MiB" }, 0,
                                                                 juce::AudioParameterChoiceAttributes().withAutomatable (false)));
        sampleMissing = testBool ("testMissing", "Test: sample missing");

        for (auto* p : getParameters())
            lastSeen.push_back (p->getValue());

        hidden.table = makeTable (0);
        lastDataChoice = testData->getIndex();
        startTimer (timerMs);
    }

    ~TestSynth() override { stopTimer(); }

    static double hzFor (float normalised)
    {
        return minHz * std::pow (maxHz / minHz, (double) normalised);
    }

    //==========================================================================
    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        smoothedLevel.reset (sr, 0.01);
        smoothedLevel.setCurrentAndTargetValue (level->get());
        smoothedHz.reset (sr, 0.01);
        smoothedHz.setCurrentAndTargetValue (hzFor (pitch->get()));
        phase = 0.0;
        audioTable = hidden.table;
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        // Take a new wavetable if one is waiting, without ever waiting for it.
        {
            const juce::SpinLock::ScopedTryLockType tryLock (tableLock);
            if (tryLock.isLocked() && newTableWaiting)
            {
                std::swap (audioTable, pendingTable);
                newTableWaiting = false;
            }
        }

        if (audioTable.size() != (size_t) tableSize)
        {
            buffer.clear();
            return;
        }

        smoothedLevel.setTargetValue (level->get());
        smoothedHz.setTargetValue (hzFor (pitch->get()));
        const bool square = wave->getIndex() == 1;
       #if TEST_SYNTH_VERSION >= 2
        const float gainIn = 1.0f + 4.0f * drive->get();
       #endif

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto pos = phase / juce::MathConstants<double>::twoPi * tableSize;
            const auto index = (int) pos;
            const auto frac = (float) (pos - index);
            auto s = audioTable[(size_t) (index % tableSize)] * (1.0f - frac) + audioTable[(size_t) ((index + 1) % tableSize)] * frac;
            if (square)
                s = s >= 0.0f ? 1.0f : -1.0f;
           #if TEST_SYNTH_VERSION >= 2
            s = std::tanh (s * gainIn) / std::tanh (gainIn);
           #endif
            s *= smoothedLevel.getNextValue();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, s);

            phase += juce::MathConstants<double>::twoPi * smoothedHz.getNextValue() / sampleRate;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
    }

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override                      { return true; }

    const juce::String getName() const override          { return JucePlugin_Name; }
    bool acceptsMidi() const override                    { return true; }
    bool producesMidi() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }
    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    // The state: a header, every real parameter by its ID, then the hidden part.

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream out (dest, false);
        out.writeInt ((int) stateMagic);
        out.writeInt (stateFormat);

        juce::Array<juce::AudioProcessorParameterWithID*> real;
        for (auto* p : getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p); withId != nullptr && ! withId->paramID.startsWith ("test"))
                real.add (withId);

        out.writeInt (real.size());
        for (auto* p : real)
        {
            out.writeString (p->paramID);
            out.writeFloat (p->getValue());
        }

        out.writeInt64 (hidden.tableSeed);
        out.writeInt ((int) hidden.data.size());
        out.write (hidden.data.data(), hidden.data.size());
        out.writeString (hidden.samplePath);
        out.writeBool (hidden.settled);
       #if TEST_SYNTH_VERSION >= 2
        out.writeString ("format 2 adds this field");
       #endif

        if (testNoisy->get())
            out.writeInt (++saves);
    }

    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream in (data, (size_t) size, false);

        if ((juce::uint32) in.readInt() != stateMagic)
            return;

        // A newer format than this version knows: refuse it and keep what is
        // loaded, without telling anyone, as many plugins do.
        if (in.readInt() > stateFormat)
            return;

        const int count = in.readInt();
        for (int i = 0; i < count && ! in.isExhausted(); ++i)
        {
            const auto id = in.readString();
            const auto value = in.readFloat();
            if (auto* p = findParam (id))
                p->setValueNotifyingHost (value);
        }

        Hidden h;
        h.tableSeed = in.readInt64();
        const auto dataSize = in.readInt();
        h.data.resize ((size_t) juce::jmax (0, dataSize));
        in.read (h.data.data(), (int) h.data.size());
        h.samplePath = in.readString();
        h.settled = in.readBool();

        // A settling plugin finishes loading later and saves that it has.
        if (testSettle->get())
        {
            h.settled = false;
            settleAtMs = juce::Time::getMillisecondCounterHiRes() + 300.0;
        }

        setHidden (std::move (h), false);
    }

private:
    struct Hidden
    {
        juce::int64 tableSeed = 0;
        std::vector<float> table;
        std::vector<char> data;
        juce::String samplePath;
        juce::MemoryBlock sample;
        bool settled = true;
    };

    juce::AudioProcessorParameterWithID* findParam (const juce::String& id) const
    {
        for (auto* p : getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p); withId != nullptr && withId->paramID == id)
                return withId;
        return nullptr;
    }

    /** Replaces the hidden part, hands the wavetable to the audio thread, and tells the host if asked to. */
    void setHidden (Hidden h, bool changedHere)
    {
        h.table = makeTable (h.tableSeed);

        if (h.samplePath.isNotEmpty())
        {
            const juce::File file (h.samplePath);
            h.sample.reset();
            if (file.existsAsFile())
                file.loadFileAsData (h.sample);
            else if (! hidden.sample.isEmpty() && hidden.samplePath == h.samplePath)
                h.sample = hidden.sample;   // already loaded here before the file went away
        }

        const bool missing = h.samplePath.isNotEmpty() && h.sample.isEmpty();
        hidden = std::move (h);

        {
            const juce::SpinLock::ScopedLockType lock (tableLock);
            pendingTable = hidden.table;
            newTableWaiting = true;
        }

        if (sampleMissing->get() != missing)
            sampleMissing->setValueNotifyingHost (missing ? 1.0f : 0.0f);

        if (changedHere && testDirty->get())
            updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    }

    // The message thread: what the plugin's own window would do.
    void timerCallback() override
    {
        if (testMove->get() && sweepTick < 0)
        {
            sweepFrom = pitch->get();
            sweepTo = sweepFrom < 0.5f ? 0.9f : 0.1f;
            sweepTick = 0;
            pitch->beginChangeGesture();
        }

        if (sweepTick >= 0)
        {
            ++sweepTick;
            const float t = (float) sweepTick / (float) sweepTicks;
            *pitch = sweepFrom + (sweepTo - sweepFrom) * t;   // setValueNotifyingHost
            lastSeen[(size_t) pitch->getParameterIndex()] = static_cast<juce::AudioProcessorParameter*> (pitch)->getValue();

            if (sweepTick >= sweepTicks)
            {
                pitch->endChangeGesture();
                sweepTick = -1;
                *testMove = false;
            }
        }

        // The hidden part's test controls.
        if (testTable->get())
        {
            *testTable = false;
            auto h = copyHidden();
            h.tableSeed = juce::Random::getSystemRandom().nextInt64() | 1;
            setHidden (std::move (h), true);
        }

        if (testPreset->get())
        {
            *testPreset = false;
            auto h = copyHidden();
            h.tableSeed = juce::Random::getSystemRandom().nextInt64() | 1;
            setHidden (std::move (h), true);
            // Moved without telling the host: no setValueNotifyingHost.
            static_cast<juce::AudioProcessorParameter*> (level)->setValue (juce::Random::getSystemRandom().nextFloat() * 0.5f + 0.5f);
            static_cast<juce::AudioProcessorParameter*> (pitch)->setValue (juce::Random::getSystemRandom().nextFloat());
            lastSeen[(size_t) level->getParameterIndex()] = static_cast<juce::AudioProcessorParameter*> (level)->getValue();
            lastSeen[(size_t) pitch->getParameterIndex()] = static_cast<juce::AudioProcessorParameter*> (pitch)->getValue();
        }

        if (testData->getIndex() != lastDataChoice)
        {
            lastDataChoice = testData->getIndex();
            static const int sizes[] { 0, 64 * 1024, 1024 * 1024, 8 * 1024 * 1024 };
            auto h = copyHidden();
            h.data.resize ((size_t) sizes[lastDataChoice]);
            auto& r = juce::Random::getSystemRandom();
            for (auto& b : h.data)
                b = (char) r.nextInt (256);
            setHidden (std::move (h), true);
        }

        if (testSample->get())
        {
            *testSample = false;
            const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("collabgod-test-synth");
            folder.createDirectory();
            const auto file = folder.getChildFile ("sample-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()) + ".wav");
            file.replaceWithText ("not really audio, just a file the state refers to");
            auto h = copyHidden();
            h.samplePath = file.getFullPathName();
            setHidden (std::move (h), true);
        }

        if (! hidden.settled && settleAtMs > 0.0 && juce::Time::getMillisecondCounterHiRes() >= settleAtMs)
        {
            settleAtMs = 0.0;
            auto h = copyHidden();
            h.settled = true;
            setHidden (std::move (h), true);
        }

        // Report host-set values back, rounded, as some plugins do.
        auto& params = getParameters();

        for (int i = 0; i < params.size(); ++i)
        {
            auto* p = params[i];
            const float v = p->getValue();

            if (juce::exactlyEqual (v, lastSeen[(size_t) i]))
                continue;

            lastSeen[(size_t) i] = v;

            if (testEcho->get() && dynamic_cast<juce::AudioProcessorParameterWithID*> (p)->paramID.startsWith ("test") == false)
            {
                const float rounded = std::round (v * 100.0f) / 100.0f;
                p->setValueNotifyingHost (rounded);
                lastSeen[(size_t) i] = p->getValue();
            }
        }
    }

    Hidden copyHidden() const
    {
        Hidden h;
        h.tableSeed = hidden.tableSeed;
        h.data = hidden.data;
        h.samplePath = hidden.samplePath;
        h.sample = hidden.sample;
        h.settled = hidden.settled;
        return h;
    }

   #if TEST_SYNTH_VERSION >= 2
    juce::AudioParameterFloat* drive = nullptr;
   #endif
    juce::AudioParameterFloat* level = nullptr;
    juce::AudioParameterFloat* pitch = nullptr;
    juce::AudioParameterChoice* wave = nullptr;
    juce::AudioParameterBool* testMove = nullptr;
    juce::AudioParameterBool* testEcho = nullptr;
    juce::AudioParameterBool* testTable = nullptr;
    juce::AudioParameterBool* testPreset = nullptr;
    juce::AudioParameterBool* testSample = nullptr;
    juce::AudioParameterBool* testDirty = nullptr;
    juce::AudioParameterBool* testNoisy = nullptr;
    juce::AudioParameterBool* testSettle = nullptr;
    juce::AudioParameterChoice* testData = nullptr;
    juce::AudioParameterBool* sampleMissing = nullptr;

    Hidden hidden;
    int lastDataChoice = 0;
    int saves = 0;
    double settleAtMs = 0.0;

    juce::SpinLock tableLock;
    std::vector<float> audioTable, pendingTable;
    bool newTableWaiting = false;

    std::vector<float> lastSeen;
    int sweepTick = -1;
    float sweepFrom = 0.0f, sweepTo = 0.0f;

    double sampleRate = 44100.0, phase = 0.0;
    juce::SmoothedValue<float> smoothedLevel;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> smoothedHz { 220.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestSynth)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestSynth();
}
