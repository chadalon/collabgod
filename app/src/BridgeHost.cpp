// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BridgeHost.h"
#include "EngineHost.h"
#include "Log.h"

#include <limits>

namespace collabgod
{
    bridge::WebView platformWebView()
    {
       #if JUCE_WINDOWS
        return bridge::WebView::WebView2;
       #elif JUCE_MAC
        return bridge::WebView::WKWebView;
       #else
        return bridge::WebView::WebKitGTK;
       #endif
    }

    namespace
    {
        bridge::AudioChoice choiceToBridge (const AudioChoice& c)
        {
            return { c.type, c.device, c.sampleRate, c.bufferSize, c.audioThreads };
        }

        AudioChoice fromBridge (const bridge::AudioChoice& c)
        {
            // Outside int's range is outside any device's, and is read as "the default".
            const auto toInt = [] (juce::int64 v) { return v > 0 && v <= std::numeric_limits<int>::max() ? static_cast<int> (v) : 0; };
            return { c.type, c.device, c.sampleRate, toInt (c.bufferSize), toInt (c.audioThreads) };
        }

        bridge::AudioState stateToBridge (AudioState state)
        {
            switch (state)
            {
                case AudioState::open:     return bridge::AudioState::open;
                case AudioState::fallback: return bridge::AudioState::fallback;
                case AudioState::none:     return bridge::AudioState::none;
            }
            return bridge::AudioState::none;
        }
    }

    bridge::AudioStatus toBridge (const AudioStatus& s)
    {
        bridge::AudioStatus out;
        out.state = stateToBridge (s.state);
        out.type = s.type;
        out.device = s.device;
        out.sampleRate = s.sampleRate;
        out.bufferSize = s.bufferSize;
        out.outputLatencyMs = s.outputLatencyMs;
        for (const auto rate : s.sampleRates)
            out.sampleRates.push_back (rate);
        for (const auto size : s.bufferSizes)
            out.bufferSizes.push_back (size);
        out.chosen = choiceToBridge (s.chosen);
        out.audioThreads = s.audioThreads;
        out.cpus = s.cpus;
        out.settled = s.settled;
        out.lateCallbacks = s.lateCallbacks;
        out.deviceDropouts = s.deviceDropouts;
        out.message = s.message;
        return out;
    }

    namespace
    {
        /** What the page is told when the app has no audio engine (the contract tests). */
        bridge::AudioStatus noEngine()
        {
            AudioStatus s;
            s.message = "The app was started without an audio engine";
            return collabgod::toBridge (s);
        }
    }

    BridgeHost::BridgeHost (HostSettings s)
        : settings (s),
          startMs (juce::Time::getMillisecondCounterHiRes()),
          delivery (s.frameDelivery)
    {
    }

    double BridgeHost::hostTimeMs() const
    {
        return juce::Time::getMillisecondCounterHiRes() - startMs;
    }

    bridge::HostInfo BridgeHost::hello()
    {
        count ("hello");

        if (! pageReady)
            collabgod::log ("Page connected over the bridge (" + bridge::toString (platformWebView())
                            + ", frames " + bridge::toString (delivery) + ")");

        pageReady = true;

        bridge::HostInfo info;
        info.contractVersion = bridge::contractVersion;
        info.appVersion = JUCE_APPLICATION_VERSION_STRING;
        info.platform = juce::SystemStats::getOperatingSystemName();
        info.webView = platformWebView();
        info.juceVersion = juce::SystemStats::getJUCEVersion();
        info.engineVersion = juce::String (COLLABGOD_TRACKTION_COMMIT).substring (0, 12);
        info.development = settings.development;
        info.frameDelivery = delivery;
        info.showDiagnostics = settings.showDiagnostics;
        info.selfCheck = settings.selfCheck;
        return info;
    }

    bridge::PingResult BridgeHost::ping (const bridge::PingParams& params)
    {
        count ("ping");
        return { params.sentAtMs, hostTimeMs() };
    }

    BridgeHost::~BridgeHost()
    {
        setEngine (nullptr);
    }

    void BridgeHost::setEngine (EngineHost* e)
    {
        if (engine != nullptr)
            engine->onStatusChanged = nullptr;

        engine = e;

        if (engine != nullptr)
        {
            engine->onStatusChanged = [this] (const AudioStatus& status)
            {
                if (pageReady && onAudioChanged != nullptr)
                    onAudioChanged (toBridge (status));
            };
        }
    }

    bridge::Frame BridgeHost::buildFrame()
    {
        bridge::Frame frame;
        frame.seq = seq++;
        frame.hostTimeMs = hostTimeMs();
        frame.lastProbe = lastProbe;
        frame.lateCallbacks = engine != nullptr ? engine->lateCallbacks() : 0;
        return frame;
    }

    bridge::AudioDevices BridgeHost::audioDevices()
    {
        count ("audioDevices");
        bridge::AudioDevices devices;

        if (engine == nullptr)
        {
            devices.status = noEngine();
            return devices;
        }

        for (const auto& type : engine->deviceTypes (true))
        {
            bridge::AudioDeviceType t;
            t.name = type.name;
            for (const auto& name : type.devices)
                t.devices.push_back (name);
            devices.types.push_back (std::move (t));
        }

        devices.status = toBridge (engine->status());
        return devices;
    }

    bridge::AudioStatus BridgeHost::chooseAudioOutput (const bridge::AudioChoice& choice)
    {
        count ("chooseAudioOutput");

        if (engine == nullptr)
            return noEngine();

        return toBridge (engine->choose (fromBridge (choice)));
    }

    bridge::Frame BridgeHost::pullFrame()
    {
        count ("pullFrame");
        return buildFrame();
    }

    bridge::FrameDeliverySetting BridgeHost::setFrameDelivery (const bridge::FrameDeliverySetting& setting)
    {
        count ("setFrameDelivery");

        if (setting.frameDelivery != delivery)
            collabgod::log ("Frames are now " + bridge::toString (setting.frameDelivery));

        delivery = setting.frameDelivery;
        return { delivery };
    }

    void BridgeHost::log (const bridge::LogParams& params)
    {
        count ("log");
        collabgod::log ("page " + bridge::toString (params.level) + ": " + params.message);
    }

    juce::String BridgeHost::summarise (const bridge::Diagnostics& d)
    {
        const auto timings = [] (const bridge::Timings& t)
        {
            return t.samples == 0 ? juce::String ("no samples")
                                  : "median " + juce::String (t.medianMs, 2) + " ms, p95 " + juce::String (t.p95Ms, 2)
                                        + " ms, max " + juce::String (t.maxMs, 2) + " ms (" + juce::String (t.samples) + ")";
        };

        return "frames " + bridge::toString (d.frameDelivery) + ", " + juce::String (d.framesPerSecond, 1) + " fps, "
             + juce::String (d.staleFrames) + " of " + juce::String (d.drawnFrames) + " drawn frames without new data; "
             + "bridge round trip " + timings (d.roundTrip) + "; probe " + timings (d.probe) + "; "
             + juce::String (d.commands) + " commands in " + juce::String (d.commandBatches) + " batches";
    }

    void BridgeHost::reportDiagnostics (const bridge::Diagnostics& d)
    {
        count ("reportDiagnostics");
        diagnostics = d;
        collabgod::log ("Diagnostics: " + summarise (d));
    }

    juce::StringArray BridgeHost::unused() const
    {
        juce::StringArray names;

        const auto check = [&] (const char* name)
        {
            const auto it = calls.find (name);
            if (it == calls.end() || it->second == 0)
                names.add (name);
        };

        for (const auto* name : bridge::functionNames)
            check (name);
        for (const auto* name : bridge::commandNames)
            check (name);

        return names;
    }

    void BridgeHost::selfCheckDone (const bridge::SelfCheckReport& report)
    {
        count ("selfCheckDone");

        bool passed = report.passed;
        collabgod::log ("Self-check from the page: " + juce::String (report.checks) + " checks, "
                        + (report.passed ? "passed" : "FAILED"));

        for (const auto& failure : report.failures)
            collabgod::log ("  FAIL " + failure);

        collabgod::log ("  " + summarise (report.diagnostics));

        // The app's half: every function and command crossed the real bridge into here.
        for (const auto& name : unused())
        {
            collabgod::log ("  FAIL the app never received " + name);
            passed = false;
        }

        if (onSelfCheckDone != nullptr)
            onSelfCheckDone (passed);
    }

    void BridgeHost::probe (const bridge::ProbeCommand& command)
    {
        count ("probe");
        lastProbe = command.id;
    }
}
