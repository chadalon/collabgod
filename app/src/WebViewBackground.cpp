// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows and Linux. macOS is WebViewBackground_mac.mm.

#include "WebViewBackground.h"

namespace collabgod
{
    void matchWebViewBackground (juce::WebBrowserComponent&, juce::Colour)
    {
        // Windows: WinWebView2::withBackgroundColour in WebUi.cpp does it.
    }
}
