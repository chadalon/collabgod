// Spike for issue #11. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>

namespace projection
{

/**
    What a recording of the engine's output says about the left channel, which should
    carry nothing but the probe (a steady sine) while the model's clips play on the right.

    Measured in 50 ms windows. In each, the probe is fitted as a sine of known frequency
    (amplitude and phase by least squares); what is left over on the left channel is
    anything that is not the probe, such as right-channel audio leaking across.

    This is what the engine hands the audio device. Anything heard that is not in it
    happens later: in the operating system's mixer, the interface or the headphones.
*/
struct OutputAnalysis
{
    double seconds = 0.0;
    int windows = 0;
    static constexpr double windowSeconds = 0.05;

    double probeLevel = 0.0;        ///< median probe amplitude over the windows (linear peak)
    double probeLowest = 0.0;       ///< lowest window's probe amplitude
    double probeLowestAt = 0.0;     ///< seconds into the recording
    int dipWindows = 0;             ///< windows where the probe is more than dipDb below its median
    static constexpr double dipDb = 1.0;

    double leftResidualMax = 0.0;   ///< loudest window of non-probe signal on the left (RMS)
    double leftResidualMaxAt = 0.0;
    /** The worst ratio of non-probe signal on the left to the right channel's level, in dB,
        over windows where the right is louder than -40 dBFS RMS. Negative infinity when the
        left is perfectly clean; NaN when the right was never that loud. */
    double crosstalkDb = std::numeric_limits<double>::quiet_NaN();
    double crosstalkAt = 0.0;

    double leftPeak = 0.0, rightPeak = 0.0;
    int samplesOverFullScale = 0;   ///< samples on either channel with magnitude above 1.0

    juce::var toVar() const;
};

OutputAnalysis analyseOutput (const juce::AudioBuffer<float>& stereo, double sampleRate, double probeHz);

/** Writes a stereo 32-bit float WAV. Returns false if it could not be written. */
bool writeRecording (const juce::File&, const juce::AudioBuffer<float>&, double sampleRate);

} // namespace projection
