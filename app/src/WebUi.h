// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BridgeHost.h"
#include "UiFiles.h"

namespace collabgod
{
    struct UiSettings
    {
        /** Where the interface's built page is (packages/interface/dist, or its copy beside the app). */
        juce::File folder;
        /** Reload the page when the folder changes. */
        bool reloadOnChange = false;
    };

    /**
        The window's content: JUCE's web view showing the interface, with the bridge's
        functions registered and frames pushed on every display refresh when the page
        asks for push. WebView2 on Windows, WKWebView on macOS (ADR-0002).

        Everything here runs on the message thread.
    */
    class WebUi final : public juce::Component,
                        private juce::Timer
    {
    public:
        WebUi (BridgeHost&, UiSettings);
        ~WebUi() override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        juce::WebBrowserComponent::Options makeOptions();
        void displayRefreshed();
        void timerCallback() override;

        BridgeHost& host;
        const UiSettings settings;
        std::unique_ptr<FolderWatcher> watcher;
        std::unique_ptr<juce::WebBrowserComponent> browser;
        std::unique_ptr<juce::VBlankAttachment> vblank;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebUi)
    };

    /** The folder the web view keeps its own data in: WebView2's profile on Windows. */
    juce::File webViewDataFolder();
}
