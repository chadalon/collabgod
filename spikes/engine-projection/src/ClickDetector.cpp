// Spike for issue #11. Throwaway code: product code never includes this.

#include "ClickDetector.h"

namespace projection
{

const char* ClickDetectorPlugin::xmlTypeName = "collabgodClickDetector";

ClickDetectorPlugin::ClickDetectorPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    for (auto& t : eventTimes)
        t.store (0.0);
}

ClickDetectorPlugin::~ClickDetectorPlugin()
{
    notifyListenersOfDeletion();
}

void ClickDetectorPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    // Called when a graph is built, possibly while the old graph still runs this
    // plugin on the audio thread, so only the atomic is touched. The carried samples
    // are deliberately kept: a discontinuity across a graph swap is what we look for.
    sampleRate.store (info.sampleRate);
}

void ClickDetectorPlugin::arm()
{
    armRequested.fetch_add (1);
}

void ClickDetectorPlugin::prepareCapture (double seconds, double rate)
{
    const auto length = static_cast<int> (seconds * rate);
    auto l = std::make_unique<std::vector<float>> (static_cast<size_t> (length), 0.0f);
    auto r = std::make_unique<std::vector<float>> (static_cast<size_t> (length), 0.0f);

    captureLength.store (0);  // the audio thread stops writing before the pointers change
    captureLeft.store (l->data());
    captureRight.store (r->data());
    captureRate = rate;
    captureStorage.push_back (std::move (l));
    captureStorage.push_back (std::move (r));
    captureLength.store (length);
}

juce::AudioBuffer<float> ClickDetectorPlugin::getCapture() const
{
    const int length = captureLength.load();
    juce::AudioBuffer<float> out (2, length);
    if (length > 0)
    {
        out.copyFrom (0, 0, captureLeft.load(), length);
        out.copyFrom (1, 0, captureRight.load(), length);
    }
    return out;
}

ClickDetectorPlugin::Counts ClickDetectorPlugin::getCounts() const
{
    Counts c;
    c.seconds = static_cast<double> (samplesWatched.load()) / sampleRate.load();
    c.probeClicks = probeClicks.load();
    c.probeGaps = probeGaps.load();
    c.maxProbeStep = maxProbeStep.load();
    c.clipEdges = clipEdges.load();

    const int n = juce::jmin (numEvents.load(), maxEvents);
    for (int i = 0; i < n; ++i)
        c.eventEditTimes.push_back (eventTimes[static_cast<size_t> (i)].load());

    return c;
}

void ClickDetectorPlugin::noteEvent (double editTime) noexcept
{
    const int index = numEvents.fetch_add (1);
    if (index < maxEvents)
        eventTimes[static_cast<size_t> (index)].store (editTime);
}

void ClickDetectorPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.destBuffer->getNumChannels() < 2)
        return;

    const int requested = armRequested.load();
    if (requested != armSeen.load())
    {
        samplesWatched.store (0);
        probeClicks.store (0);
        probeGaps.store (0);
        clipEdges.store (0);
        numEvents.store (0);
        maxProbeStep.store (0.0f);
        left = {};
        right = {};
        warmUp = 3;
        armSeen.store (requested);
    }

    if (requested == 0)
        return;  // never armed: watch nothing

    const int start = fc.bufferStartSample, num = fc.bufferNumSamples;
    const float* l = fc.destBuffer->getReadPointer (0, start);
    const float* r = fc.destBuffer->getReadPointer (1, start);
    const double rate = sampleRate.load();
    const double blockStartTime = fc.editTime.getStart().inSeconds();
    float maxStep = maxProbeStep.load();

    for (int i = 0; i < num; ++i)
    {
        const float x = l[i], y = r[i];

        if (warmUp > 0)
        {
            --warmUp;
        }
        else
        {
            const float step = std::abs (x - 2.0f * left.prev1 + left.prev2);
            maxStep = juce::jmax (maxStep, step);

            if (left.holdOff > 0)
                --left.holdOff;

            if (step > clickStep && left.holdOff == 0)
            {
                probeClicks.fetch_add (1);
                noteEvent (blockStartTime + i / rate);
                left.holdOff = eventHoldOffSamples;
            }

            left.silentRun = std::abs (x) < silentLevel ? left.silentRun + 1 : 0;
            if (left.silentRun == gapSamples && left.holdOff == 0)
            {
                probeGaps.fetch_add (1);
                noteEvent (blockStartTime + i / rate);
                left.holdOff = eventHoldOffSamples;
            }

            const float rightStep = std::abs (y - 2.0f * right.prev1 + right.prev2);
            if (right.holdOff > 0)
                --right.holdOff;
            if (rightStep > clipEdgeStep && right.holdOff == 0)
            {
                clipEdges.fetch_add (1);
                right.holdOff = eventHoldOffSamples;
            }
        }

        left.prev2 = left.prev1;   left.prev1 = x;
        right.prev2 = right.prev1; right.prev1 = y;
    }

    maxProbeStep.store (maxStep);
    samplesWatched.fetch_add (num);

    // Capture by edit time, so a check can ask "what played at 2.5 s on the timeline".
    const int length = captureLength.load();
    if (fc.isPlaying && length > 0)
    {
        float* cl = captureLeft.load();
        float* cr = captureRight.load();
        const auto first = static_cast<int64_t> (std::llround (blockStartTime * rate));

        for (int i = 0; i < num; ++i)
        {
            const auto index = first + i;
            if (index >= 0 && index < length)
            {
                cl[index] = l[i];
                cr[index] = r[i];
            }
        }
    }
}

} // namespace projection
