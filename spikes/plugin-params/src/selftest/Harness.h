// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.
//
// What the self-tests share: a headless engine, a simulated sound card, members
// connected over TCP on this machine, checks, and listening to a render.

#pragma once

#include <JuceHeader.h>
#include <iostream>

#include "../host/Link.h"
#include "../host/PluginSession.h"

namespace spike::harness
{
namespace te = tracktion;

struct HeadlessBehaviour : te::EngineBehaviour
{
    bool autoInitialiseDeviceManager() override { return false; }
};

constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;

/** Stands in for a sound card, as in spikes/webview-ui: calls the engine's hosted
    device one block at a time, paced to real time. Plugins only take values the
    host sets when they process audio, so something must keep them processing. */
class SimulatedSoundCard final : public juce::Thread
{
public:
    explicit SimulatedSoundCard (te::HostedAudioDeviceInterface& h)
        : juce::Thread ("Simulated sound card"), hosted (h), buffer (2, blockSize)
    {
        midi.ensureSize (1024);
    }

    ~SimulatedSoundCard() override { stopThread (2000); }

    void run() override
    {
        const double blockMs = 1000.0 * blockSize / sampleRate;
        double next = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            buffer.clear();
            midi.clear();
            hosted.processBlock (buffer, midi);

            next += blockMs;
            const double wait = next - juce::Time::getMillisecondCounterHiRes();

            if (wait >= 1.0)
                juce::Thread::sleep (static_cast<int> (wait));
            else if (wait < -100.0)
                next = juce::Time::getMillisecondCounterHiRes();
        }
    }

private:
    te::HostedAudioDeviceInterface& hosted;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};

inline int failures = 0;

inline void print (const juce::String& s)
{
    std::cout << s << std::endl;
}

inline void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    print ((ok ? "PASS " : "FAIL ") + what + (detail.isNotEmpty() ? " (" + detail + ")" : juce::String()));
    if (! ok)
        ++failures;
}

inline void pump (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

inline bool waitFor (const std::function<bool()>& done, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

    while (! done())
    {
        if (juce::Time::getMillisecondCounterHiRes() > end)
            return false;
        pump (5);
    }

    return true;
}

inline bool near (float a, float b, float tol) { return std::abs (a - b) <= tol; }

/** One member: a session and its connection. */
struct Member
{
    Member (te::Engine& engine, const juce::String& id, juce::StringArray& logLines)
        : session (engine, id)
    {
        session.log = [&logLines] (const juce::String& s) { logLines.add (s); print ("  log: " + s); };
        session.send = [this] (const juce::String& m) { link.send (m); };
        link.onMessage = [this] (const juce::String& m) { session.receive (m); };
        link.onPeersChanged = [this]
        {
            const int n = link.numConnections();
            if (n < connections)
                session.peersLeft();
            connections = n;
        };
    }

    /** Opens the plugin's own window. On Linux a JUCE plugin's timers, which make
        its own changes here as a mouse would, only run while its window is open:
        they run on the host's run loop, which the VST3 host hands over with the
        window. On Windows and macOS they run without one. */
    void openWindowOnLinux()
    {
       #if JUCE_LINUX
        if (auto* instance = session.getInstance())
        {
            if (auto* editor = instance->createEditorAndMakeActive())
            {
                window = std::make_unique<juce::DocumentWindow> (session.getMember(), juce::Colours::black, 0);
                window->setContentOwned (editor, true);
                window->setVisible (true);
            }
        }
       #endif
    }

    ~Member()
    {
        window.reset();
    }

    float value (const juce::String& id) const
    {
        auto* p = session.findParam (id);
        return p != nullptr ? p->jp->getValue() : -1.0f;
    }

    juce::String idOf (const juce::String& name) const
    {
        for (auto& p : session.getParams())
            if (p.name == name)
                return p.id;
        return {};
    }

    PluginSession session;
    spike::Link link;
    int connections = 0;
    std::unique_ptr<juce::DocumentWindow> window;
};

/** What a render of a member's edit sounds like. */
struct Sound
{
    double rmsDb = -200.0;
    double hz = 0.0;
};

/** A sweep by the host's own control, as dragging a slider in this app would make it. */
inline void hostSweep (Member& m, const juce::String& id, float from, float to, int steps, int stepMs)
{
    m.session.hostGestureBegin (id);

    for (int i = 0; i <= steps; ++i)
    {
        m.session.hostSet (id, from + (to - from) * (float) i / (float) steps);
        pump (stepMs);
    }

    m.session.hostGestureEnd (id);
}

/** Renders a second of a member's edit offline and reads it back. */
inline juce::AudioBuffer<float> render (Member& m, const juce::File& folder, const juce::String& name, double& sampleRateOut)
{
    auto& edit = m.session.getEdit();
    const auto file = folder.getChildFile (name + ".wav");
    file.deleteFile();
    edit.dispatchPendingUpdatesSynchronously();

    // Render offline, out of the live playback graph, then put the graph back.
    const te::TransportControl::ScopedContextAllocator restore (edit.getTransport());
    edit.getTransport().freePlaybackContext();

    const bool ok = te::Renderer::renderToFile ({}, file, edit, { te::TimePosition(), te::TimePosition::fromSeconds (1.0) },
                                                te::toBitSet (te::getAllTracks (edit)), true, false, {}, false);
    if (! ok)
        return {};

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return {};

    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
    sampleRateOut = reader->sampleRate;
    return buffer;
}

inline Sound listen (Member& m, const juce::File& folder, const juce::String& name)
{
    double sr = 44100.0;
    const auto buffer = render (m, folder, name, sr);
    Sound s;

    // Skip the first 0.2 s: the plugin's smoothing settles there.
    const int start = (int) (0.2 * sr);
    const int n = buffer.getNumSamples() - start;

    if (n <= 0)
        return s;

    const auto* data = buffer.getReadPointer (0);
    double sum = 0.0;
    int crossings = 0;

    for (int i = start; i < buffer.getNumSamples(); ++i)
    {
        sum += (double) data[i] * data[i];
        if (i > start && data[i - 1] < 0.0f && data[i] >= 0.0f)
            ++crossings;
    }

    const double rms = std::sqrt (sum / n);
    s.rmsDb = rms > 0.0 ? 20.0 * std::log10 (rms) : -200.0;
    s.hz = crossings / ((double) n / sr);
    return s;
}

/** The levels of the first eight harmonics of `hz`, relative to the strongest:
    the tone's colour, whatever its phase. Two members with the same wavetable
    give the same numbers. */
inline std::vector<double> timbre (Member& m, const juce::File& folder, const juce::String& name, double hz)
{
    double sr = 44100.0;
    const auto buffer = render (m, folder, name, sr);
    std::vector<double> out;
    const int start = (int) (0.3 * sr);
    const int n = buffer.getNumSamples() - start;

    if (n <= 0)
        return out;

    const auto* data = buffer.getReadPointer (0) + start;

    for (int k = 1; k <= 8; ++k)
    {
        // Goertzel at k times the fundamental.
        const double w = juce::MathConstants<double>::twoPi * hz * k / sr;
        const double c = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double s0 = data[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        out.push_back (std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n);
    }

    const double peak = *std::max_element (out.begin(), out.end());
    for (auto& v : out)
        v = peak > 0.0 ? v / peak : 0.0;
    return out;
}

/** The largest difference between two timbres. */
inline double timbreDistance (const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size() || a.empty())
        return 1.0;

    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

} // namespace spike::harness
