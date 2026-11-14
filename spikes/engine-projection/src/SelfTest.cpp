// Spike for issue #11. Throwaway code: product code never includes this.
//
// `engine_projection selftest`: checks the engine side with no audio device, using a
// simulated sound card on a real-time thread of our own. ctest runs it.
//
//   A. Contract: every example in contract/examples gives the readback it says.
//   B. Edits during playback change what plays: clips moved, removed and added half a
//      second before the playhead reaches them are heard where they now are, and only there.
//   C. A storm of edits while looping: 8 s of random moves, resizes, removals and additions,
//      many right at the playhead. The probe must stay clean: no click, no gap.
//   D. Rebuild from scratch while playing: reset, then add everything back in one batch.
//      The readback matches, and the probe stays clean.

#include <iostream>

#include "Commands.h"
#include "Host.h"
#include "ToneCheck.h"
#include "Wav.h"

namespace projection
{

namespace
{
void print (const juce::String& s) { std::cout << s << std::endl; }

/** Runs the message loop and the host's tick until `done`, or the time-out. */
bool pumpUntil (Host& host, const std::function<bool()>& done, double timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (! done())
    {
        if (juce::Time::getMillisecondCounterHiRes() > deadline)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        host.tick();
    }
    return true;
}

void pumpFor (Host& host, double ms)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + ms;
    pumpUntil (host, [&] { return juce::Time::getMillisecondCounterHiRes() >= until; }, ms + 1000.0);
}

/** Structural equality, numbers within a microsecond. Explains the first difference. */
bool sameJson (const juce::var& a, const juce::var& b, const juce::String& path, juce::String& why)
{
    const auto isNum = [] (const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); };

    if (isNum (a) && isNum (b))
    {
        if (std::abs (static_cast<double> (a) - static_cast<double> (b)) <= 1.0e-6)
            return true;
        why = path + ": " + a.toString() + " != " + b.toString();
        return false;
    }

    if (a.isArray() && b.isArray())
    {
        if (a.size() != b.size())
        {
            why = path + ": " + juce::String (a.size()) + " items != " + juce::String (b.size());
            return false;
        }
        for (int i = 0; i < a.size(); ++i)
            if (! sameJson (a[i], b[i], path + "[" + juce::String (i) + "]", why))
                return false;
        return true;
    }

    auto* oa = a.getDynamicObject();
    auto* ob = b.getDynamicObject();
    if (oa != nullptr && ob != nullptr)
    {
        if (oa->getProperties().size() != ob->getProperties().size())
        {
            why = path + ": different keys";
            return false;
        }
        for (auto& p : oa->getProperties())
        {
            if (! ob->hasProperty (p.name))
            {
                why = path + ": missing " + p.name.toString();
                return false;
            }
            if (! sameJson (p.value, ob->getProperty (p.name), path + "." + p.name.toString(), why))
                return false;
        }
        return true;
    }

    if (a.isString() && b.isString() && a.toString() == b.toString())
        return true;
    if (a.isVoid() && b.isVoid())
        return true;

    why = path + ": " + juce::JSON::toString (a, true) + " != " + juce::JSON::toString (b, true);
    return false;
}

juce::var cmd (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto o = std::make_unique<juce::DynamicObject>();
    for (auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o.release());
}

struct Timings
{
    std::vector<double> apply, rebuild;

    void add (const Projector::ApplyResult& r) { apply.push_back (r.applyMs); rebuild.push_back (r.rebuildMs); }

    static juce::String describe (std::vector<double> v)
    {
        if (v.empty())
            return "none";
        std::sort (v.begin(), v.end());
        return "median " + juce::String (v[v.size() / 2], 2) + " ms, p95 "
               + juce::String (v[std::min (v.size() - 1, v.size() * 95 / 100)], 2) + " ms, max "
               + juce::String (v.back(), 2) + " ms";
    }

    juce::String describe() const
    {
        return juce::String (static_cast<int> (apply.size())) + " batches. Apply: " + describe (apply)
               + ". Graph rebuild: " + describe (rebuild);
    }
};

struct Samples
{
    juce::File a, b, c;   // 660, 880 and 1100 Hz, 2 s each
};

class SelfTest
{
public:
    SelfTest (Host& h, Projector& p, Samples s) : host (h), projector (p), samples (s) {}

    bool failed = false;

    void check (bool ok, const juce::String& what)
    {
        print (juce::String (ok ? "  ok   " : "  FAIL ") + what);
        failed = failed || ! ok;
    }

    bool apply (const juce::Array<juce::var>& commands, Timings* timings = nullptr)
    {
        const auto r = projector.apply (commands);
        if (timings != nullptr)
            timings->add (r);
        if (r.failedAt >= 0)
            check (false, "command " + juce::String (r.failedAt) + " failed: " + r.error);
        return r.failedAt < 0;
    }

    //==============================================================================
    void contract()
    {
        print ("A. Contract examples");
        const juce::File dir (ENGINE_PROJECTION_CONTRACT_DIR);
        auto files = dir.getChildFile ("examples").findChildFiles (juce::File::findFiles, false, "*.json");
        files.sort();
        check (files.size() > 0, "found " + juce::String (files.size()) + " examples in " + dir.getFullPathName());

        // Paths are sent in the platform's own form (contract/README.md), so "${samples}/" becomes
        // the folder and the native separator. They go into JSON strings, so backslashes are escaped.
        const auto samplesDir = (samples.a.getParentDirectory().getFullPathName() + juce::File::getSeparatorString())
                                    .replace ("\\", "\\\\");

        for (auto& file : files)
        {
            const auto example = juce::JSON::parse (file.loadFileAsString().replace ("${samples}/", samplesDir));
            projector.apply (juce::Array<juce::var> { cmd ({ { "op", "reset" } }) });
            const auto r = projector.apply (example["commands"]);
            const int expectedFailure = example.hasProperty ("failedAt") ? static_cast<int> (example["failedAt"]) : -1;

            juce::String why;
            const bool sameFailure = r.failedAt == expectedFailure;
            const bool sameDoc = sameJson (projector.readback(), example["readback"], "readback", why);
            check (sameFailure && sameDoc, file.getFileName()
                   + (sameFailure ? "" : ": failed at " + juce::String (r.failedAt) + " (" + r.error + "), expected "
                                         + juce::String (expectedFailure))
                   + (sameDoc ? "" : ": " + why));
        }
    }

    //==============================================================================
    void editsChangeWhatPlays()
    {
        print ("B. Edits during playback change what plays");
        const auto file = [] (const juce::File& f) { return juce::var (f.getFullPathName()); };

        apply ({ cmd ({ { "op", "reset" } }),
                 cmd ({ { "op", "track.add" }, { "id", "t1" }, { "after", juce::var() } }),
                 cmd ({ { "op", "track.add" }, { "id", "t2" }, { "after", "t1" } }),
                 cmd ({ { "op", "clip.add" }, { "id", "x" }, { "trackId", "t1" }, { "start", 1.0 }, { "length", 1.0 },
                        { "sampleHash", "660" }, { "file", file (samples.a) } }),
                 cmd ({ { "op", "clip.add" }, { "id", "y" }, { "trackId", "t2" }, { "start", 2.0 }, { "length", 1.0 },
                        { "sampleHash", "880" }, { "file", file (samples.b) } }) });

        auto& detector = host.getDetector();
        const double rate = host.getAudioInfo().sampleRate;
        detector.prepareCapture (5.0, rate);
        host.setLoop (0.0, 0.0);
        host.setPosition (0.0);
        pumpFor (host, 100.0);
        detector.arm();
        host.setPlaying (true);

        check (pumpUntil (host, [&] { return host.getEdit().getTransport().isPlaying() && host.getAudibleSeconds() >= 0.5; }, 5000.0),
               "transport playing and past 0.5 s");
        const double editAt = host.getAudibleSeconds();

        // Half a second before the first clip: move it to 3 s on the other track, remove
        // the second, and add a new one on a new track where the second was.
        Timings timings;
        apply ({ cmd ({ { "op", "clip.set" }, { "id", "x" }, { "trackId", "t2" }, { "start", 3.0 } }),
                 cmd ({ { "op", "clip.remove" }, { "id", "y" } }),
                 cmd ({ { "op", "track.add" }, { "id", "t3" }, { "after", "t2" } }),
                 cmd ({ { "op", "clip.add" }, { "id", "z" }, { "trackId", "t3" }, { "start", 2.0 }, { "length", 1.0 },
                        { "sampleHash", "1100" }, { "file", file (samples.c) } }) },
               &timings);
        print ("  edits applied at " + juce::String (editAt, 3) + " s: " + timings.describe());

        check (pumpUntil (host, [&] { return host.getAudibleSeconds() >= 4.2; }, 8000.0), "played to 4.2 s");
        host.setPlaying (false);
        pumpFor (host, 50.0);

        const auto capture = detector.getCapture();
        const auto measure = [&] (int channel, double from, double to, double hz)
        {
            const int start = static_cast<int> (from * rate), end = static_cast<int> (to * rate);
            return spike::toneAmplitude (capture.getReadPointer (channel, start), end - start, rate, hz);
        };

        const auto db = [] (double a) { return juce::String (juce::Decibels::gainToDecibels (a, -120.0), 1) + " dB"; };
        constexpr double present = 0.1, absent = 0.0025;  // clips are 0.25 peak; absent means 40 dB down

        struct Window { const char* what; double from, to, hz; bool shouldPlay; };
        const Window windows[] {
            { "1-2 s, where x was: no 660 Hz", 1.1, 1.9, 660.0, false },
            { "1-2 s: no 880 Hz", 1.1, 1.9, 880.0, false },
            { "1-2 s: no 1100 Hz", 1.1, 1.9, 1100.0, false },
            { "2-3 s, where y was: no 880 Hz (removed)", 2.1, 2.9, 880.0, false },
            { "2-3 s: 1100 Hz (z, added)", 2.1, 2.9, 1100.0, true },
            { "2-3 s: no 660 Hz", 2.1, 2.9, 660.0, false },
            { "3-4 s: 660 Hz (x, moved here)", 3.1, 3.9, 660.0, true },
            { "3-4 s: no 1100 Hz", 3.1, 3.9, 1100.0, false },
        };

        for (auto& w : windows)
        {
            const auto amp = measure (1, w.from, w.to, w.hz);
            check (w.shouldPlay ? amp > present : amp < absent, juce::String ("right: ") + w.what + ", " + db (amp));
        }

        const auto probe = measure (0, 0.6, 4.0, Host::probeHz);
        check (probe > 0.1, "left: the probe plays throughout, " + db (probe));
        const auto probeLeak = measure (0, 3.1, 3.9, 660.0);
        check (probeLeak < absent, "left: no clip audio on the probe's side, " + db (probeLeak));

        reportDetector ("B");
    }

    //==============================================================================
    struct MirrorClip { juce::String track; double start, length; int sample; };

    juce::var addClipCommand (const juce::String& id, const MirrorClip& c) const
    {
        const juce::File* files[] { &samples.a, &samples.b, &samples.c };
        static const char* hashes[] { "660", "880", "1100" };
        return cmd ({ { "op", "clip.add" }, { "id", id }, { "trackId", c.track }, { "start", c.start },
                      { "length", c.length }, { "sampleHash", hashes[c.sample] },
                      { "file", files[c.sample]->getFullPathName() } });
    }

    juce::Array<juce::var> fullDocument() const
    {
        juce::Array<juce::var> commands { cmd ({ { "op", "reset" } }) };
        juce::var after;
        for (auto& t : trackOrder)
        {
            commands.add (cmd ({ { "op", "track.add" }, { "id", t }, { "after", after } }));
            after = t;
        }
        for (auto& [id, c] : mirror)
            commands.add (addClipCommand (id, c));
        return commands;
    }

    juce::var expectedReadback() const
    {
        const juce::File* files[] { &samples.a, &samples.b, &samples.c };
        static const char* hashes[] { "660", "880", "1100" };
        juce::Array<juce::var> tracks;
        for (auto& t : trackOrder)
        {
            juce::Array<juce::var> clips;
            for (auto& [id, c] : mirror)  // std::map: sorted by id, as the readback is
                if (c.track == t)
                    clips.add (cmd ({ { "id", id }, { "start", c.start }, { "length", c.length }, { "offset", 0.0 },
                                      { "sampleHash", hashes[c.sample] }, { "file", files[c.sample]->getFullPathName() } }));
            tracks.add (cmd ({ { "id", t }, { "clips", clips } }));
        }
        return cmd ({ { "tracks", tracks } });
    }

    void checkReadback (const juce::String& when)
    {
        juce::String why;
        check (sameJson (projector.readback(), expectedReadback(), "readback", why),
               "readback matches what was sent " + when + (why.isEmpty() ? "" : ": " + why));
    }

    void storm()
    {
        print ("C. A storm of edits while looping 0-4 s");
        juce::Random random (11);
        constexpr double loopEnd = 4.0;

        trackOrder.clear();
        mirror.clear();
        for (int t = 0; t < 12; ++t)
            trackOrder.add ("s" + juce::String (t));
        for (int i = 0; i < 60; ++i)
            mirror["c" + juce::String (nextClip++)] = randomClip (random, loopEnd, -1.0);

        apply (fullDocument());
        checkReadback ("before playing");

        host.setLoop (0.0, loopEnd);
        host.setPosition (0.0);
        host.setPlaying (true);
        pumpUntil (host, [&] { return host.getEdit().getTransport().isPlaying(); }, 5000.0);
        pumpFor (host, 200.0);

        // The same playback with no edits, to compare the sound card's timing against.
        host.resetBlockTimes();
        pumpFor (host, 3000.0);
        const double quietMaxBlock = host.getMaxBlockMs();
        const double quietMaxCpu = host.getMaxBlockCpuMs();
        const int quietLate = host.getLateBlocks();

        host.getDetector().arm();
        host.resetBlockTimes();
        const int stopsBefore = host.getUnexpectedStops();

        Timings timings;
        int commandsSent = 0;
        const auto stormEnd = juce::Time::getMillisecondCounterHiRes() + 8000.0;

        while (juce::Time::getMillisecondCounterHiRes() < stormEnd && ! failed)
        {
            juce::Array<juce::var> batch;
            const int n = 1 + random.nextInt (4);
            const double playhead = host.getAudibleSeconds();

            for (int k = 0; k < n; ++k)
                batch.add (randomEdit (random, loopEnd, playhead));

            commandsSent += batch.size();
            apply (batch, &timings);
            pumpFor (host, 50.0);
        }

        print ("  " + juce::String (commandsSent) + " commands; " + timings.describe());
        const auto budget = juce::String (1000.0 * Host::simulatedBlockSize / Host::simulatedSampleRate, 1);
        print ("  simulated sound card, " + juce::String (host.getAudioCpus()) + " CPUs for audio of "
               + juce::String (juce::SystemStats::getNumCpus()) + ", blocks of " + budget + " ms:");
        print ("    3 s with no edits: slowest block " + juce::String (quietMaxBlock, 2) + " ms (CPU " + juce::String (quietMaxCpu, 2) + " ms), " + juce::String (quietLate) + " late");
        print ("    8 s of edits:      slowest block " + juce::String (host.getMaxBlockMs(), 2) + " ms (CPU " + juce::String (host.getMaxBlockCpuMs(), 2) + " ms), "
               + juce::String (host.getLateBlocks()) + " late (not a pass or fail here: see README)");
        check (host.getEdit().getTransport().isPlaying() && host.getUnexpectedStops() == stopsBefore,
               "the transport kept playing without being pressed again");
        checkReadback ("after the storm");
        reportDetector ("C");
    }

    void rebuildWhilePlaying()
    {
        print ("D. Rebuild everything from scratch while playing");
        Timings timings;
        apply (fullDocument(), &timings);
        print ("  reset and " + juce::String (static_cast<int> (mirror.size())) + " clips on "
               + juce::String (trackOrder.size()) + " tracks: " + timings.describe());
        checkReadback ("after the rebuild");
        pumpFor (host, 1000.0);
        check (host.getEdit().getTransport().isPlaying(), "still playing");
        reportDetector ("C and D");
        host.setPlaying (false);
    }

private:
    Host& host;
    Projector& projector;
    Samples samples;
    juce::StringArray trackOrder;
    std::map<juce::String, MirrorClip> mirror;
    int nextClip = 0;

    MirrorClip randomClip (juce::Random& random, double loopEnd, double near)
    {
        MirrorClip c;
        c.track = trackOrder[random.nextInt (trackOrder.size())];
        // Half the time right at the playhead, so the edit lands on something sounding.
        c.start = near >= 0.0 && random.nextBool() ? juce::jmax (0.0, near - 0.2 + random.nextDouble() * 0.4)
                                                   : random.nextDouble() * loopEnd;
        c.start = std::round (c.start * 1000.0) / 1000.0;
        c.length = 0.05 + std::round (random.nextDouble() * 1500.0) / 1000.0;
        c.sample = random.nextInt (3);
        return c;
    }

    juce::var randomEdit (juce::Random& random, double loopEnd, double playhead)
    {
        const int kind = random.nextInt (10);

        if (mirror.empty() || kind == 0)
        {
            const auto id = "c" + juce::String (nextClip++);
            mirror[id] = randomClip (random, loopEnd, playhead);
            return addClipCommand (id, mirror[id]);
        }

        auto it = mirror.begin();
        std::advance (it, random.nextInt (static_cast<int> (mirror.size())));
        const auto id = it->first;
        auto& c = it->second;

        if (kind == 1)
        {
            mirror.erase (it);
            return cmd ({ { "op", "clip.remove" }, { "id", id } });
        }

        if (kind < 5)
        {
            const auto moved = randomClip (random, loopEnd, playhead);
            c.track = moved.track;
            c.start = moved.start;
            return cmd ({ { "op", "clip.set" }, { "id", id }, { "trackId", c.track }, { "start", c.start } });
        }

        if (kind < 8)
        {
            const auto moved = randomClip (random, loopEnd, playhead);
            c.start = moved.start;
            return cmd ({ { "op", "clip.set" }, { "id", id }, { "start", c.start } });
        }

        c.length = 0.05 + std::round (random.nextDouble() * 1500.0) / 1000.0;
        return cmd ({ { "op", "clip.set" }, { "id", id }, { "length", c.length } });
    }

    void reportDetector (const juce::String& phase)
    {
        const auto c = host.getDetector().getCounts();
        juce::String events;
        for (auto t : c.eventEditTimes)
            events << " " << juce::String (t, 3);

        print ("  detector (" + phase + "): watched " + juce::String (c.seconds, 1) + " s, largest probe step "
               + juce::String (c.maxProbeStep, 5) + " (click above " + juce::String (ClickDetectorPlugin::clickStep, 3)
               + "), " + juce::String (c.clipEdges) + " clip edges on the right");
        check (c.seconds > 1.0, "the detector watched the audio");
        check (c.probeClicks == 0 && c.probeGaps == 0,
               "the probe is clean: " + juce::String (c.probeClicks) + " clicks, " + juce::String (c.probeGaps) + " gaps"
               + (events.isEmpty() ? "" : ", at edit time" + events));
    }
};
} // namespace

int selfTest (const juce::File& outFolder, Options options)
{
    outFolder.createDirectory();
    const auto sampleDir = outFolder.getChildFile ("samples");
    const Samples samples { sampleDir.getChildFile ("a.wav"), sampleDir.getChildFile ("b.wav"), sampleDir.getChildFile ("c.wav") };

    if (! writeSineWav (samples.a, 660.0, 0.25f, 2.0, 44100.0) || ! writeSineWav (samples.b, 880.0, 0.25f, 2.0, 44100.0)
        || ! writeSineWav (samples.c, 1100.0, 0.25f, 2.0, 44100.0))
    {
        print ("FAIL could not write test samples in " + sampleDir.getFullPathName());
        return 1;
    }

    Host host (Host::Audio::simulated, options.audioCpus);
    Projector projector (host, options.realTimeNodes);
    print (juce::String ("Clips played by ") + (options.realTimeNodes ? "WaveNodeRealTime (no proxies)" : "WaveNode (Tracktion's default)"));
    SelfTest test (host, projector, samples);

    const bool settled = pumpUntil (host, [&] { return host.isSettled(); }, 10000.0);
    test.check (settled, "engine settled after " + juce::String (host.getSettleMs(), 0) + " ms");

    test.contract();
    if (! test.failed) test.editsChangeWhatPlays();
    if (! test.failed) test.storm();
    if (! test.failed) test.rebuildWhilePlaying();

    print (test.failed ? "FAIL" : "PASS");
    return test.failed ? 1 : 0;
}

} // namespace projection
