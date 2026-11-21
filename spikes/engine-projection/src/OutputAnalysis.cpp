// Spike for issue #11. Throwaway code: product code never includes this.

#include "OutputAnalysis.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace projection
{

namespace
{
double db (double ratio) { return ratio > 0.0 ? 20.0 * std::log10 (ratio) : -std::numeric_limits<double>::infinity(); }

/** JSON has no infinity or NaN: write null for them. */
juce::var number (double v) { return std::isfinite (v) ? juce::var (v) : juce::var(); }
} // namespace

OutputAnalysis analyseOutput (const juce::AudioBuffer<float>& stereo, double sampleRate, double probeHz)
{
    OutputAnalysis a;
    const int n = stereo.getNumChannels() >= 2 ? stereo.getNumSamples() : 0;
    a.seconds = n / sampleRate;

    const float* left = n > 0 ? stereo.getReadPointer (0) : nullptr;
    const float* right = n > 0 ? stereo.getReadPointer (1) : nullptr;

    for (int i = 0; i < n; ++i)
    {
        const auto l = std::abs (left[i]), r = std::abs (right[i]);
        a.leftPeak = std::max (a.leftPeak, static_cast<double> (l));
        a.rightPeak = std::max (a.rightPeak, static_cast<double> (r));
        a.samplesOverFullScale += (l > 1.0f ? 1 : 0) + (r > 1.0f ? 1 : 0);
    }

    const int window = static_cast<int> (std::lround (OutputAnalysis::windowSeconds * sampleRate));
    a.windows = window > 0 ? n / window : 0;
    if (a.windows == 0)
        return a;

    const double w = juce::MathConstants<double>::twoPi * probeHz / sampleRate;
    std::vector<double> amplitudes (static_cast<size_t> (a.windows));
    double worstRatio = -1.0;

    for (int k = 0; k < a.windows; ++k)
    {
        const int start = k * window;

        // Least-squares fit of x = p sin + q cos over the window.
        double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0, rr = 0;
        for (int i = start; i < start + window; ++i)
        {
            const double s = std::sin (w * i), c = std::cos (w * i), x = left[i];
            ss += s * s; sc += s * c; cc += c * c; xs += x * s; xc += x * c;
            rr += static_cast<double> (right[i]) * right[i];
        }
        const double det = ss * cc - sc * sc;
        const double p = det != 0.0 ? (xs * cc - xc * sc) / det : 0.0;
        const double q = det != 0.0 ? (xc * ss - xs * sc) / det : 0.0;

        double residual = 0;
        for (int i = start; i < start + window; ++i)
        {
            const double e = left[i] - (p * std::sin (w * i) + q * std::cos (w * i));
            residual += e * e;
        }

        const double amplitude = std::sqrt (p * p + q * q);
        const double residualRms = std::sqrt (residual / window);
        const double rightRms = std::sqrt (rr / window);
        const double at = start / sampleRate;
        amplitudes[static_cast<size_t> (k)] = amplitude;

        if (k == 0 || amplitude < a.probeLowest)
        {
            a.probeLowest = amplitude;
            a.probeLowestAt = at;
        }
        if (residualRms > a.leftResidualMax)
        {
            a.leftResidualMax = residualRms;
            a.leftResidualMaxAt = at;
        }
        if (rightRms > 0.01 && residualRms / rightRms > worstRatio)  // 0.01 RMS is -40 dBFS
        {
            worstRatio = residualRms / rightRms;
            a.crosstalkAt = at;
        }
    }

    auto sorted = amplitudes;
    std::nth_element (sorted.begin(), sorted.begin() + static_cast<long> (sorted.size() / 2), sorted.end());
    a.probeLevel = sorted[sorted.size() / 2];

    const double dipBelow = a.probeLevel * std::pow (10.0, -OutputAnalysis::dipDb / 20.0);
    for (auto amplitude : amplitudes)
        a.dipWindows += amplitude < dipBelow ? 1 : 0;

    if (worstRatio >= 0.0)
        a.crosstalkDb = db (worstRatio);

    return a;
}

juce::var OutputAnalysis::toVar() const
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("seconds", seconds);
    o->setProperty ("windows", windows);
    o->setProperty ("windowSeconds", windowSeconds);
    o->setProperty ("probeDbfs", number (db (probeLevel)));
    o->setProperty ("probeLowestDbfs", number (db (probeLowest)));
    o->setProperty ("probeLowestAt", probeLowestAt);
    o->setProperty ("dipWindows", dipWindows);
    o->setProperty ("dipDb", dipDb);
    o->setProperty ("leftResidualMaxDbfs", number (db (leftResidualMax)));
    o->setProperty ("leftResidualMaxAt", leftResidualMaxAt);
    o->setProperty ("crosstalkDb", number (crosstalkDb));
    o->setProperty ("crosstalkAt", crosstalkAt);
    o->setProperty ("leftPeakDbfs", number (db (leftPeak)));
    o->setProperty ("rightPeakDbfs", number (db (rightPeak)));
    o->setProperty ("samplesOverFullScale", samplesOverFullScale);
    return juce::var (o.release());
}

bool writeRecording (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (buffer.getNumChannels())
                                                   .withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

} // namespace projection
