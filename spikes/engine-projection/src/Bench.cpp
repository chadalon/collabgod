// Spike for issue #11. Throwaway code: product code never includes this.
//
// `engine_projection bench`: what an edit costs on the engine side as the session
// grows, while playing through a simulated sound card. Prints a Markdown table.

#include <iostream>

#include "Commands.h"
#include "Host.h"
#include "Wav.h"

namespace projection
{

namespace
{
void print (const juce::String& s) { std::cout << s << std::endl; }

juce::var cmd (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto o = std::make_unique<juce::DynamicObject>();
    for (auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o.release());
}

void pumpFor (Host& host, double ms)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + ms;
    while (juce::Time::getMillisecondCounterHiRes() < until)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        host.tick();
    }
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}
} // namespace

int bench (const juce::File& outFolder, bool deviceAudio, Options options)
{
    const auto sample = outFolder.getChildFile ("bench-660.wav");
    if (! writeSineWav (sample, 660.0, 0.1f, 2.0, 44100.0))
    {
        print ("Could not write " + sample.getFullPathName());
        return 1;
    }

    Host host (deviceAudio ? Host::Audio::device : Host::Audio::simulated, options.audioCpus, options.deviceType, options.bufferSize);
    Projector projector (host, options.realTimeNodes);

    while (! host.isSettled())
        pumpFor (host, 10.0);

    host.setLoop (0.0, 8.0);
    host.setPlaying (true);
    pumpFor (host, 300.0);

    const auto info = host.getAudioInfo();
    print ("Audio: " + info.type + " / " + info.name + ", " + juce::String (info.blockSize) + " samples at "
           + juce::String (info.sampleRate) + " Hz, " + juce::String (host.getAudioCpus()) + " CPUs for audio of "
           + juce::String (juce::SystemStats::getNumCpus()) + ", clips played by "
           + (options.realTimeNodes ? "WaveNodeRealTime (no proxies)" : "WaveNode (Tracktion's default)") + ", playing a loop while editing");
    print ("");
    print ("| Clips | Tracks | Add all in one batch | Its graph rebuild | One move: apply | One move: graph rebuild | Readback | Slowest block, no edits (late) | Slowest block, moves (late) | Probe clicks |");
    print ("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |");

    constexpr int numTracks = 50;
    juce::Random random (7);

    for (int numClips : { 100, 1000, 5000 })
    {
        juce::Array<juce::var> batch { cmd ({ { "op", "reset" } }) };
        juce::var after;
        for (int t = 0; t < numTracks; ++t)
        {
            batch.add (cmd ({ { "op", "track.add" }, { "id", "t" + juce::String (t) }, { "after", after } }));
            after = "t" + juce::String (t);
        }
        for (int i = 0; i < numClips; ++i)
            batch.add (cmd ({ { "op", "clip.add" }, { "id", "c" + juce::String (i) },
                              { "trackId", "t" + juce::String (i % numTracks) },
                              { "start", (i / numTracks) * 0.25 }, { "length", 0.2 },
                              { "sampleHash", "660" }, { "file", sample.getFullPathName() } }));

        const auto big = projector.apply (batch);
        if (big.failedAt >= 0)
        {
            print ("Failed: " + big.error);
            return 1;
        }

        pumpFor (host, 300.0);

        // A second of the same session with no edits, for comparison.
        host.resetBlockTimes();
        pumpFor (host, 1000.0);
        const double quietMaxBlock = host.getMaxBlockMs();
        const double quietMaxCpu = host.getMaxBlockCpuMs();
        const int quietLate = host.getLateBlocks();

        host.getDetector().arm();
        host.resetBlockTimes();
        pumpFor (host, 50.0);

        std::vector<double> apply, rebuild;
        for (int k = 0; k < 20; ++k)
        {
            const auto r = projector.apply (juce::Array<juce::var> {
                cmd ({ { "op", "clip.set" }, { "id", "c" + juce::String (random.nextInt (numClips)) },
                       { "start", random.nextInt (32) * 0.25 } }) });
            apply.push_back (r.applyMs);
            rebuild.push_back (r.rebuildMs);
            pumpFor (host, 50.0);
        }

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto doc = projector.readback();
        const auto readbackMs = juce::Time::getMillisecondCounterHiRes() - t0;
        juce::ignoreUnused (doc);

        const auto counts = host.getDetector().getCounts();
        print ("| " + juce::String (numClips) + " | " + juce::String (numTracks) + " | "
               + juce::String (big.applyMs, 1) + " ms | " + juce::String (big.rebuildMs, 1) + " ms | "
               + juce::String (median (apply), 2) + " ms (max " + juce::String (*std::max_element (apply.begin(), apply.end()), 2) + ") | "
               + juce::String (median (rebuild), 2) + " ms (max " + juce::String (*std::max_element (rebuild.begin(), rebuild.end()), 2) + ") | "
               + juce::String (readbackMs, 1) + " ms | "
               + (deviceAudio ? juce::String ("n/a") : juce::String (quietMaxBlock, 2) + " ms, CPU " + juce::String (quietMaxCpu, 2) + " ms (" + juce::String (quietLate) + ")") + " | "
               + (deviceAudio ? juce::String ("n/a") : juce::String (host.getMaxBlockMs(), 2) + " ms, CPU " + juce::String (host.getMaxBlockCpuMs(), 2) + " ms (" + juce::String (host.getLateBlocks()) + ")") + " | "
               + juce::String (counts.probeClicks + counts.probeGaps) + " |");
    }

    host.setPlaying (false);
    print ("");
    print ("Moves are 20 single-clip moves, 50 ms apart, each timed to the end of the graph rebuild it causes. "
           "Slowest block: the simulated sound card's slowest processBlock, against a budget of "
           + juce::String (1000.0 * Host::simulatedBlockSize / Host::simulatedSampleRate, 1) + " ms; CPU is the audio thread's own CPU time in that block, the rest being time it waited or was preempted; late is how many blocks went over the budget. "
           "Probe clicks cover the moves only.");
    return 0;
}

} // namespace projection
