// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.

#pragma once

namespace cg
{
/** Whether any mouse button is down anywhere on screen, as the system sees it.

    JUCE's ModifierKeys::getCurrentModifiersRealtime() asks the system on Windows and
    Linux, but on macOS it learns about buttons only from mouse events that reach JUCE's
    own windows. A plugin's editor is a native view that takes its own clicks, so a knob
    held in it went unseen there (finding 16). On macOS this asks AppKit instead. */
bool anyMouseButtonDown();
}
