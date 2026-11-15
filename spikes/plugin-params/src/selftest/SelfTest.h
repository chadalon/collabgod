// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>

namespace spike
{
/** Runs the self-test with the app's command line arguments. Returns the exit code. */
int runSelfTest (const juce::StringArray& args);
}
