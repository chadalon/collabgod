// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.
//
// A VST3 instrument that plays a steady tone, so the self-test can check that a
// mirrored parameter change changes the sound without any MIDI. Built twice:
// version 1 and version 2, which adds a parameter at the front of the list, so
// every shared parameter has a different index but the same ID. The sync must
// survive that (it matches parameters by ID, never by index).
//
// Two controls that are not automatable let a host make the plugin behave like
// a real one would, without a window or a mouse:
// - "Test: move pitch" makes the plugin sweep its own Pitch over one second,
//   with a gesture, exactly as dragging a knob in its window would report it.
// - "Test: echo host changes" makes it report every value the host sets back to
//   the host, rounded to two decimals, as some plugins do.
// Not being automatable, both are skipped by Tracktion Engine and never synced.

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
} // namespace

class TestSynth final : public juce::AudioProcessor,
                        private juce::Timer
{
public:
    TestSynth()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
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
        addParameter (testMove = new juce::AudioParameterBool (juce::ParameterID { "testMove", 1 }, "Test: move pitch", false,
                                                               juce::AudioParameterBoolAttributes().withAutomatable (false)));
        addParameter (testEcho = new juce::AudioParameterBool (juce::ParameterID { "testEcho", 1 }, "Test: echo host changes", false,
                                                               juce::AudioParameterBoolAttributes().withAutomatable (false)));

        for (auto* p : getParameters())
            lastSeen.push_back (p->getValue());

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
        smoothedLevel.setTargetValue (level->get());
        smoothedHz.setTargetValue (hzFor (pitch->get()));
        const bool square = wave->getIndex() == 1;
       #if TEST_SYNTH_VERSION >= 2
        const float gainIn = 1.0f + 4.0f * drive->get();
       #endif

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            auto s = (float) std::sin (phase);
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

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream out (dest, false);
        for (auto* p : getParameters())
            out.writeFloat (p->getValue());
    }

    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        for (auto* p : getParameters())
            if (! in.isExhausted())
                p->setValueNotifyingHost (in.readFloat());
    }

private:
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

        // Report host-set values back, rounded, as some plugins do.
        auto& params = getParameters();

        for (int i = 0; i < params.size(); ++i)
        {
            auto* p = params[i];
            const float v = p->getValue();

            if (juce::exactlyEqual (v, lastSeen[(size_t) i]))
                continue;

            lastSeen[(size_t) i] = v;

            if (testEcho->get() && p != testEcho && p != testMove)
            {
                const float rounded = std::round (v * 100.0f) / 100.0f;
                p->setValueNotifyingHost (rounded);
                lastSeen[(size_t) i] = p->getValue();
            }
        }
    }

   #if TEST_SYNTH_VERSION >= 2
    juce::AudioParameterFloat* drive = nullptr;
   #endif
    juce::AudioParameterFloat* level = nullptr;
    juce::AudioParameterFloat* pitch = nullptr;
    juce::AudioParameterChoice* wave = nullptr;
    juce::AudioParameterBool* testMove = nullptr;
    juce::AudioParameterBool* testEcho = nullptr;

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
