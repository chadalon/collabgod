// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Only the module it needs, not JuceHeader.h: that adds `using namespace juce`, and
// WebViewBackground_mac.mm also sees Carbon's global Component, which would clash.
#include <juce_gui_extra/juce_gui_extra.h>

/*  The window, the web view and the page share one colour, so nothing white shows before
    the page draws or while the window resizes (webview-ui finding 11). Linux, which is
    not a product platform, does none of this. */
namespace collabgod
{
    /** Call before creating the web view.
        Windows: tells WebView2 the colour for its own window, which it reads as it starts.
        macOS: nothing. */
    void beforeCreatingWebView (juce::Colour);

    /** Call once the web view exists.
        Windows: nothing; WinWebView2::withBackgroundColour and beforeCreatingWebView do it.
        macOS: turns off WKWebView's own background, so the window's shows through. */
    void matchWebViewBackground (juce::WebBrowserComponent&, juce::Colour);

    /** Call once the top-level window is on screen, with the window.
        Windows: fills the area a resize exposes with the colour at once, instead of the
        white Windows shows until JUCE draws on the next display refresh.
        macOS: nothing; the window's own background colour already does this. */
    void matchWindowBackground (juce::Component& window, juce::Colour);
}
