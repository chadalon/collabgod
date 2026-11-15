// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>

namespace spike
{
/** Runs the self-test with the app's command line arguments. Returns the exit code. */
int runSelfTest (const juce::StringArray& args);
/** Runs the plugin state self-test (#13). Returns the exit code. */
int runStateSelfTest (const juce::StringArray& args);
}
