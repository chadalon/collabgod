// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BridgeContract.h"

#include <functional>
#include <map>
#include <optional>

namespace collabgod
{
    struct HostSettings
    {
        /** The page comes from a folder on disk and reloads when it changes. */
        bool development = false;
        /** ADR-0016: pull is the default until the comparison on both real machines decides. */
        bridge::FrameDelivery frameDelivery = bridge::FrameDelivery::pull;
        bool showDiagnostics = false;
        bool selfCheck = false;
    };

    /**
        The app's side of the bridge contract: what it answers when the page calls, the
        frames it sends, and what it does with commands. It knows nothing of the window or
        the web view, so the contract tests drive it directly.

        Everything here runs on the message thread.
    */
    class BridgeHost final : public bridge::Handlers
    {
    public:
        explicit BridgeHost (HostSettings);

        // bridge::Handlers
        bridge::HostInfo hello() override;
        bridge::PingResult ping (const bridge::PingParams&) override;
        bridge::Frame pullFrame() override;
        bridge::FrameDeliverySetting setFrameDelivery (const bridge::FrameDeliverySetting&) override;
        void log (const bridge::LogParams&) override;
        void reportDiagnostics (const bridge::Diagnostics&) override;
        void selfCheckDone (const bridge::SelfCheckReport&) override;
        void probe (const bridge::ProbeCommand&) override;

        /** The next frame, numbered. */
        bridge::Frame buildFrame();

        /** Whether frames should be pushed now: the page has said hello and asked for push. */
        bool shouldPush() const { return pageReady && delivery == bridge::FrameDelivery::push; }

        /** The page is being reloaded; nothing is pushed until it says hello again. */
        void pageUnloaded() { pageReady = false; }

        bridge::FrameDelivery frameDelivery() const { return delivery; }

        /** The page's latest diagnostics, once it has sent some. */
        const std::optional<bridge::Diagnostics>& latestDiagnostics() const { return diagnostics; }

        /** Functions and commands in the contract that the page has not used since the app started. */
        juce::StringArray unused() const;

        /** Called when the page reports its self-check, with whether it passed on both sides. */
        std::function<void (bool passed)> onSelfCheckDone;

        /** Milliseconds since this host was made. */
        double hostTimeMs() const;

        /** One line summarising diagnostics, for the log. */
        static juce::String summarise (const bridge::Diagnostics&);

    private:
        void count (const char* name) { ++calls[name]; }

        const HostSettings settings;
        const double startMs;
        bridge::FrameDelivery delivery;
        bool pageReady = false;
        juce::int64 seq = 0;
        juce::int64 lastProbe = -1;
        std::optional<bridge::Diagnostics> diagnostics;
        std::map<juce::String, int> calls;

        JUCE_DECLARE_NON_COPYABLE (BridgeHost)
    };

    /** The web view this platform uses. */
    bridge::WebView platformWebView();
}
