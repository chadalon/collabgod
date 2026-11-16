// Spike for issue #11. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>

namespace projection
{

/** Writes a mono 16-bit sine wave file. Returns false if it could not be written. */
bool writeSineWav (const juce::File&, double hz, float amplitude, double seconds, double sampleRate);

} // namespace projection
