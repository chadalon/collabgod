// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Only the module it needs, not JuceHeader.h: that adds `using namespace juce`, and
// WebViewBackground_mac.mm also sees Carbon's global Component, which would clash.
#include <juce_gui_extra/juce_gui_extra.h>

namespace collabgod
{
    /** Stops the web view painting its own background before the page draws or while the
        window resizes, so the window's colour, which matches the page, shows instead
        (webview-ui finding 11).

        On Windows WebView2 takes the colour as an option, so this does nothing there.
        On macOS it turns off WKWebView's own background. On Linux, which is not a
        product platform, it does nothing. */
    void matchWebViewBackground (juce::WebBrowserComponent&, juce::Colour);
}
