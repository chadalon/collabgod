// Spike for issue #11. Throwaway code: product code never includes this.
//
// `engine_projection serve`: the engine as a process that the TypeScript side drives,
// one JSON request per line on stdin, one JSON reply per line on stdout. Logs go to
// stderr. The messages are the ones the web view bridge (#8) would carry in the app;
// stdio stands in for the bridge here so the projection can be tested from Node
// without a window. See contract/README.md.

#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "Commands.h"
#include "Host.h"
#include "OutputAnalysis.h"

namespace projection
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto o = std::make_unique<juce::DynamicObject>();
    for (auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o.release());
}

void reply (const juce::var& seq, juce::var body)
{
    if (auto o = body.getDynamicObject())
        o->setProperty ("seq", seq);
    std::cout << juce::JSON::toString (body, juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none)) << std::endl;
}

juce::var detectorStats (const ClickDetectorPlugin& detector)
{
    const auto c = detector.getCounts();
    juce::Array<juce::var> events;
    for (auto t : c.eventEditTimes)
        events.add (t);

    return object ({ { "armed", detector.isArmed() },
                     { "seconds", c.seconds },
                     { "probeClicks", c.probeClicks },
                     { "probeGaps", c.probeGaps },
                     { "maxProbeStep", c.maxProbeStep },
                     { "clipEdges", c.clipEdges },
                     { "eventEditTimes", events } });
}

/** Reads stdin on its own thread, so the message thread never waits on it. */
struct LineReader
{
    std::mutex lock;
    std::deque<std::string> lines;
    std::atomic<bool> ended { false };

    void start()
    {
        std::thread ([this]
        {
            std::string line;
            while (std::getline (std::cin, line))
            {
                std::lock_guard<std::mutex> g (lock);
                lines.push_back (line);
            }
            ended.store (true);
        }).detach();  // getline cannot be interrupted; the process exits around it
    }

    bool next (std::string& out)
    {
        std::lock_guard<std::mutex> g (lock);
        if (lines.empty())
            return false;
        out = std::move (lines.front());
        lines.pop_front();
        return true;
    }
};
} // namespace

int serve (bool simulated, Options options)
{
    Host host (simulated ? Host::Audio::simulated : Host::Audio::device, options.audioCpus, options.deviceType, options.bufferSize);
    Projector projector (host, options.realTimeNodes);
    static LineReader reader;  // outlives the detached thread's last use
    reader.start();

    bool quit = false;

    const auto handle = [&] (const std::string& line)
    {
        const auto request = juce::JSON::parse (juce::String::fromUTF8 (line.c_str()));
        const auto seq = request["seq"];
        const auto type = request["type"].toString();

        if (type == "hello")
        {
            const auto info = host.getAudioInfo();
            reply (seq, object ({ { "ok", true },
                                  { "audio", object ({ { "simulated", info.simulated }, { "type", info.type },
                                                       { "name", info.name }, { "sampleRate", info.sampleRate },
                                                       { "blockSize", info.blockSize },
                                                       { "audioCpus", host.getAudioCpus() },
                                                       { "machineCpus", juce::SystemStats::getNumCpus() } }) },
                                  { "probeHz", Host::probeHz } }));
        }
        else if (type == "apply")
        {
            const auto r = projector.apply (request["commands"]);
            auto body = object ({ { "ok", r.failedAt < 0 }, { "applied", r.applied },
                                  { "applyMs", r.applyMs }, { "rebuildMs", r.rebuildMs },
                                  { "clips", projector.getNumClips() } });
            if (r.failedAt >= 0)
            {
                body.getDynamicObject()->setProperty ("failedAt", r.failedAt);
                body.getDynamicObject()->setProperty ("error", r.error);
            }
            reply (seq, body);
        }
        else if (type == "readback")
        {
            reply (seq, object ({ { "ok", true }, { "doc", projector.readback() } }));
        }
        else if (type == "transport")
        {
            const auto loop = request["loop"];
            if (loop.isArray() && loop.size() == 2)
                host.setLoop (loop[0], loop[1]);
            else if (request.hasProperty ("loop"))
                host.setLoop (0.0, 0.0);

            if (request.hasProperty ("position"))
                host.setPosition (request["position"]);
            if (request.hasProperty ("play"))
                host.setPlaying (request["play"]);

            reply (seq, object ({ { "ok", true } }));
        }
        else if (type == "arm")
        {
            host.getDetector().arm();
            host.resetBlockTimes();
            reply (seq, object ({ { "ok", true } }));
        }
        else if (type == "stats")
        {
            auto& transport = host.getEdit().getTransport();
            reply (seq, object ({ { "ok", true },
                                  { "settled", host.isSettled() },
                                  { "settleMs", host.getSettleMs() },
                                  { "playing", transport.isPlaying() },
                                  { "audibleSec", host.getAudibleSeconds() },
                                  { "unexpectedStops", host.getUnexpectedStops() },
                                  { "xruns", host.getXRuns() },
                                  { "cpu", host.getEngine().getDeviceManager().getCpuUsage() },
                                  { "maxBlockMs", host.getMaxBlockMs() },
                                  { "lateBlocks", host.getLateBlocks() },
                                  { "detector", detectorStats (host.getDetector()) } }));
        }
        else if (type == "record")
        {
            const double seconds = juce::jlimit (1.0, 600.0, static_cast<double> (request["seconds"]));
            host.getDetector().prepareRecording (seconds, host.getAudioInfo().sampleRate);
            reply (seq, object ({ { "ok", true }, { "seconds", seconds } }));
        }
        else if (type == "recording")
        {
            auto& detector = host.getDetector();
            const auto recording = detector.getRecording();
            const double rate = detector.getRecordingSampleRate();
            const auto analysis = analyseOutput (recording, rate, Host::probeHz);
            auto body = object ({ { "ok", true }, { "sampleRate", rate }, { "analysis", analysis.toVar() } });

            if (const auto path = request["file"].toString(); path.isNotEmpty())
            {
                const juce::File file (path);
                const bool written = writeRecording (file, recording, rate);
                body.getDynamicObject()->setProperty ("file", written ? juce::var (file.getFullPathName()) : juce::var());
                if (! written)
                    body.getDynamicObject()->setProperty ("error", "could not write " + file.getFullPathName());
            }
            reply (seq, body);
        }
        else if (type == "quit")
        {
            reply (seq, object ({ { "ok", true } }));
            quit = true;
        }
        else
        {
            reply (seq, object ({ { "ok", false }, { "error", "unknown request type '" + type + "'" } }));
        }
    };

    log ("Serving: one JSON request per line on stdin");

    while (! quit)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        host.tick();

        std::string line;
        while (! quit && reader.next (line))
            if (! juce::String (line).trim().isEmpty())
                handle (line);

        if (reader.ended.load())
        {
            // Drain anything left, then stop.
            while (! quit && reader.next (line))
                if (! juce::String (line).trim().isEmpty())
                    handle (line);
            quit = true;
        }
    }

    host.setPlaying (false);
    return 0;
}

} // namespace projection
