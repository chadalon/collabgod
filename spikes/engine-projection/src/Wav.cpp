// Spike for issue #11. Throwaway code: product code never includes this.

#include "Wav.h"

namespace projection
{

bool writeSineWav (const juce::File& file, double hz, float amplitude, double seconds, double sampleRate)
{
    const auto numSamples = static_cast<int> (seconds * sampleRate);
    juce::AudioBuffer<float> buffer (1, numSamples);

    for (int i = 0; i < numSamples; ++i)
        buffer.setSample (0, i, amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * hz * i / sampleRate)));

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (1)
                                                   .withBitsPerSample (16));

    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
}

} // namespace projection
