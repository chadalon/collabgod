// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.

#include "MouseButtons.h"

#include <JuceHeader.h>

namespace cg
{
bool anyMouseButtonDown()
{
    return juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown();
}
}
