// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "WebViewBackground.h"

#import <WebKit/WebKit.h>

namespace collabgod
{
    void matchWebViewBackground (juce::WebBrowserComponent& browser, juce::Colour colour)
    {
        // JUCE hosts the WKWebView in an NSViewComponent, the browser's only child.
        for (auto* child : browser.getChildren())
        {
            auto* host = dynamic_cast<juce::NSViewComponent*> (child);
            if (host == nullptr)
                continue;

            auto* view = static_cast<NSView*> (host->getView());
            if (view == nil || ! [view isKindOfClass: [WKWebView class]])
                continue;

            auto* webView = static_cast<WKWebView*> (view);

            // WKWebView has no public setting for this before macOS 12. Turning off its own
            // background leaves it transparent, so the window behind it shows through.
            [webView setValue: @NO forKey: @"drawsBackground"];

            // Shown when the page is scrolled or stretched past its edge.
            webView.underPageBackgroundColor = [NSColor colorWithSRGBRed: colour.getFloatRed()
                                                                   green: colour.getFloatGreen()
                                                                    blue: colour.getFloatBlue()
                                                                   alpha: 1.0];
        }
    }
}
