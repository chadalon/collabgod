// Spike for issue #11. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

namespace projection
{
namespace te = tracktion;

/**
    A plugin on the master bus that watches the output for dropouts, on the audio
    thread, so an edit that glitches the audio is measured rather than only heard.

    The left channel carries the probe: a steady 440 Hz sine on a track that no edit
    touches (the model's tracks are panned hard right). A clean sine changes smoothly,
    so two things can only come from the engine:

    - a click: the second difference of the signal jumps far above what a sine of
      this level and pitch can produce (a skipped, repeated or restarted block);
    - a gap: the signal sits at zero for 16 samples or more (a silent block).

    The right channel carries the model's clips. Clips start and end abruptly, so jumps
    there are counted for information only ("clip edges").

    Optionally records both channels by edit time while the transport plays, so a test
    can check what played where on the timeline, whatever the start-up delay was.

    Real-time safety: applyToBuffer reads and writes only atomics, members owned by the
    audio thread, and capture buffers allocated beforehand on the message thread. It never
    locks, allocates or logs.
*/
class ClickDetectorPlugin : public te::Plugin
{
public:
    explicit ClickDetectorPlugin (te::PluginCreationInfo);
    ~ClickDetectorPlugin() override;

    static const char* getPluginName()                      { return "CollabGod click detector"; }
    static const char* xmlTypeName;

    juce::String getName() const override                   { return getPluginName(); }
    juce::String getPluginType() override                   { return xmlTypeName; }
    juce::String getSelectableDescription() override        { return getPluginName(); }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    //==============================================================================
    // Message thread.

    /** Resets the counts. Takes effect at the audio thread's next block. */
    void arm();
    bool isArmed() const noexcept   { return armSeen.load() == armRequested.load(); }

    /** Records the output by edit time from 0 to `seconds` while playing. Allocates; call before playing. */
    void prepareCapture (double seconds, double sampleRate);

    /** Copies what was captured (left, right). */
    juce::AudioBuffer<float> getCapture() const;
    double getCaptureSampleRate() const noexcept    { return captureRate; }

    static constexpr int maxEvents = 16;

    struct Counts
    {
        double seconds = 0.0;          ///< audio watched since arming
        int probeClicks = 0;
        int probeGaps = 0;
        float maxProbeStep = 0.0f;     ///< largest second difference seen on the probe
        int clipEdges = 0;
        std::vector<double> eventEditTimes;  ///< edit time of the first few probe events
    };

    Counts getCounts() const;

    /** Thresholds. A 0.2 sine at 440 Hz and 44.1 kHz has a second difference of at most 0.0008. */
    static constexpr float clickStep = 0.02f;
    static constexpr float clipEdgeStep = 0.05f;
    static constexpr float silentLevel = 0.001f;
    static constexpr int gapSamples = 16;
    static constexpr int eventHoldOffSamples = 256;

private:
    std::atomic<double> rate { 44100.0 };  // not Plugin::sampleRate, which is not atomic

    std::atomic<int> armRequested { 0 }, armSeen { 0 };
    std::atomic<int64_t> samplesWatched { 0 };
    std::atomic<int> probeClicks { 0 }, probeGaps { 0 }, clipEdges { 0 }, numEvents { 0 };
    std::atomic<float> maxProbeStep { 0.0f };
    std::array<std::atomic<double>, maxEvents> eventTimes {};

    // Audio thread only.
    struct Channel { float prev1 = 0.0f, prev2 = 0.0f; int silentRun = 0; int holdOff = 0; };
    Channel left, right;
    int warmUp = 0;

    // Capture: allocated on the message thread, then published.
    std::vector<std::unique_ptr<std::vector<float>>> captureStorage;  // message thread; keeps old buffers alive
    std::atomic<float*> captureLeft { nullptr }, captureRight { nullptr };
    std::atomic<int> captureLength { 0 };
    double captureRate = 0.0;

    void noteEvent (double editTime) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClickDetectorPlugin)
};

} // namespace projection
