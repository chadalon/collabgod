// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "WebUi.h"
#include "Log.h"
#include "WebViewBackground.h"

namespace collabgod
{
    juce::File webViewDataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("CollabGod")
            .getChildFile ("WebView");
    }

    WebUi::WebUi (BridgeHost& h, UiSettings s) : host (h), settings (std::move (s))
    {
        setOpaque (true);

        browser = std::make_unique<juce::WebBrowserComponent> (makeOptions());
        matchWebViewBackground (*browser, juce::Colour (bridge::backgroundArgb));
        addAndMakeVisible (*browser);
        browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

        // Frames are pushed in step with the display, not on a timer (webview-ui finding 6).
        vblank = std::make_unique<juce::VBlankAttachment> (this, [this] { displayRefreshed(); });

        if (settings.reloadOnChange)
        {
            watcher = std::make_unique<FolderWatcher> (settings.folder);
            startTimer (250);
            collabgod::log ("Development: the page reloads when " + settings.folder.getFullPathName() + " changes");
        }
    }

    WebUi::~WebUi()
    {
        stopTimer();
        vblank.reset();
    }

    void WebUi::resized()
    {
        browser->setBounds (getLocalBounds());
    }

    juce::WebBrowserComponent::Options WebUi::makeOptions()
    {
        using Options = juce::WebBrowserComponent::Options;

        const auto folder = settings.folder;
        const auto background = juce::Colour (bridge::backgroundArgb);

        auto options = Options {}
            // Ask for WebView2 explicitly: JUCE's default on Windows is still Internet Explorer (ADR-0002).
            .withBackend (Options::Backend::webview2)
            .withWinWebView2Options (Options::WinWebView2 {}
                                         .withUserDataFolder (webViewDataFolder())
                                         .withBackgroundColour (background)
                                         .withStatusBarDisabled()
                                         .withBuiltInErrorPageDisabled())
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withNativeIntegrationEnabled()
            .withResourceProvider ([folder] (const juce::String& path) -> std::optional<juce::WebBrowserComponent::Resource>
            {
                if (auto resource = serveUiFile (folder, path))
                    return resource;

                if (path == "/" || path.startsWith ("/index.html"))
                    return missingUiPage (folder);

                collabgod::log ("The page asked for " + path + ", which is not in " + folder.getFullPathName());
                return std::nullopt;
            });

        return bridge::withHandlers (options, host);
    }

    void WebUi::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (bridge::backgroundArgb));
    }

    void WebUi::displayRefreshed()
    {
        if (host.shouldPush())
            bridge::emitFrame (*browser, host.buildFrame());
    }

    void WebUi::timerCallback()
    {
        if (watcher != nullptr && watcher->poll())
        {
            collabgod::log ("The interface changed; reloading the page");
            host.pageUnloaded();
            browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
        }
    }
}
