// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

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
