// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.
//
//   plugin_params_spike --self-test --v1 <test synth 1.vst3> --v2 <test synth 2.vst3> [--log-dir <folder>]
//
// Three members on this machine, each with its own Tracktion Engine edit and
// its own copy of the test VST3, connected over TCP on 127.0.0.1 the way two
// app instances would be. Checks that a change moves the other members' plugins
// and changes their sound, from the host's controls and from the plugin itself;
// that echoes and two people on one control do not loop; that automation is not
// sent; and that a member with version 2 of the plugin, whose parameters are at
// other indexes, is warned about and still synced by ID. Prints the message
// rates during a fast sweep. Needs no audio device and no window.

#include "Harness.h"
#include "SelfTest.h"

namespace
{
using namespace spike::harness;
using spike::PluginSession;

struct Snapshot
{
    spike::SessionStats session;
    psync::Stats sync;
    std::uint64_t bytes = 0;
    double ms = 0.0;
};

Snapshot snap (Member& m)
{
    return { m.session.getStats(), m.session.getSync().stats(), m.session.bytesSent(), PluginSession::nowMs() };
}

void printRates (const juce::String& label, const Snapshot& a, const Snapshot& b)
{
    const double s = (b.ms - a.ms) / 1000.0;
    auto rate = [s] (std::uint64_t x, std::uint64_t y) { return juce::String ((double) (y - x) / s, 1); };

    print ("RATE " + label + ": over " + juce::String (s, 2) + " s"
           + ", plugin reports " + rate (a.session.rawValueEvents, b.session.rawValueEvents) + "/s"
           + " (" + juce::String ((juce::int64) (b.session.rawValueEventsOffMessageThread - a.session.rawValueEventsOffMessageThread)) + " off the message thread)"
           + ", reach the sync " + rate (a.session.trackedEvents, b.session.trackedEvents) + "/s"
           + ", changes sent " + rate (a.sync.sentChanges, b.sync.sentChanges) + "/s"
           + " in " + rate (a.sync.sentBatches, b.sync.sentBatches) + " messages/s"
           + ", " + rate (a.bytes, b.bytes) + " bytes/s"
           + "; in all " + juce::String ((juce::int64) (b.session.trackedEvents - a.session.trackedEvents)) + " changes in, "
           + juce::String ((juce::int64) (b.sync.sentChanges - a.sync.sentChanges)) + " out, "
           + juce::String ((juce::int64) (b.bytes - a.bytes)) + " bytes");
}

int run (const juce::File& v1, const juce::File& v2, const juce::File& logDir)
{
    te::Engine engine ("CollabGodPluginParamsSpike", nullptr, std::make_unique<HeadlessBehaviour>());
    engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
    logDir.createDirectory();

    te::HostedAudioDeviceInterface::Parameters deviceParams;
    deviceParams.sampleRate = sampleRate;
    deviceParams.blockSize = blockSize;
    deviceParams.inputChannels = 0;
    deviceParams.outputChannels = 2;
    auto& hosted = engine.getDeviceManager().getHostedAudioDeviceInterface();
    hosted.initialise (deviceParams);
    hosted.prepareToPlay (sampleRate, blockSize);
    engine.getDeviceManager().dispatchPendingUpdates();
    SimulatedSoundCard soundCard (hosted);

    juce::StringArray logA, logB, logC;
    Member a (engine, "member-a", logA), b (engine, "member-b", logB);
    a.session.setEventLog (logDir.getChildFile ("events-a.csv"));
    b.session.setEventLog (logDir.getChildFile ("events-b.csv"));

    juce::String error;
    check (a.session.loadPlugin (v1, error), "A loads the test synth version 1", error);
    check (b.session.loadPlugin (v1, error), "B loads the test synth version 1", error);

    if (failures > 0)
        return 1;

    a.openWindowOnLinux();
    b.openWindowOnLinux();

    // Tracktion runs plugins while the transport is stopped, once a playback context exists.
    a.session.getEdit().getTransport().ensureContextAllocated();
    b.session.getEdit().getTransport().ensureContextAllocated();
    soundCard.startThread (juce::Thread::Priority::highest);
    pump (300);

    const auto level = a.idOf ("Level"), pitch = a.idOf ("Pitch"), wave = a.idOf ("Wave");
    check (level.isNotEmpty() && pitch.isNotEmpty() && wave.isNotEmpty(), "the synced parameters include Level, Pitch and Wave");
    bool testControlSynced = false;
    for (auto& p : a.session.getParams())
        testControlSynced = testControlSynced || p.name.startsWith ("Test:");
    check (! testControlSynced && a.session.findParameterByName ("Test: move pitch") != nullptr,
           "the plugin's two test controls are not automatable, so they are not synced",
           juce::String ((int) a.session.getParams().size()) + " synced");
    check (level != juce::String (a.session.findParam (level)->index),
           "parameters are keyed by the plugin's own ID, not their index", "Level is ID " + level + " at index "
           + juce::String (a.session.findParam (level)->index));

    const int port = a.link.host (0);
    check (port > 0, "A listens", "port " + juce::String (port));
    check (b.link.join ("127.0.0.1", port), "B connects to A");
    waitFor ([&] { return a.link.numConnections() == 1; }, 2000);
    b.session.sendHello();
    check (waitFor ([&] { return a.session.getPeers().size() == 1 && b.session.getPeers().size() == 1; }, 2000),
           "A and B exchange hellos");
    check (a.session.getMismatches().begin()->second.isEmpty(), "same plugin and version: no parameter list warning");

    //==========================================================================
    print ("\n1. A change from the host's controls");
    const auto before = listen (b, logDir, "b-before");
    hostSweep (a, level, 0.25f, 0.8f, 20, 10);
    waitFor ([&] { return near (b.value (level), 0.8f, 1.0e-4f); }, 2000);
    check (near (b.value (level), 0.8f, 1.0e-4f), "B's Level follows A's", juce::String (b.value (level), 4));
    const auto afterA = listen (a, logDir, "a-after-level"), afterB = listen (b, logDir, "b-after-level");
    check (afterB.rmsDb - before.rmsDb > 8.0, "B's sound gets louder",
           juce::String (before.rmsDb, 1) + " dB to " + juce::String (afterB.rmsDb, 1) + " dB; expected about +10.1");
    check (std::abs (afterA.rmsDb - afterB.rmsDb) < 0.1, "A and B sound the same",
           juce::String (afterA.rmsDb, 2) + " dB and " + juce::String (afterB.rmsDb, 2) + " dB");

    //==========================================================================
    print ("\n2. A change from the plugin itself, as its own window would report it");
    const auto pitchBefore = listen (b, logDir, "b-before-pitch");
    const auto s0 = snap (a);
    auto* move = a.session.findParameterByName ("Test: move pitch");
    check (move != nullptr, "the test synth has its move control");
    if (move != nullptr)
        move->setValue (1.0f);   // as a host sets any parameter; the plugin then moves Pitch itself
    const float pitchStart = a.value (pitch);
    // The plugin's timer asks for 5 ms ticks, so the sweep should take a second, but
    // timers can run slower (about 37 ticks a second on a macOS CI runner): wait for
    // the end of its gesture, not a fixed time.
    waitFor ([&] { return snap (a).session.rawGestureEvents - s0.session.rawGestureEvents >= 2; }, 20000);
    pump (300);
    const auto s1 = snap (a);
    check (s1.session.rawGestureEvents - s0.session.rawGestureEvents >= 2, "the plugin reports a gesture around its own change",
           juce::String ((juce::int64) (s1.session.rawGestureEvents - s0.session.rawGestureEvents)) + " gesture events");
    check (std::abs (a.value (pitch) - pitchStart) > 0.5f, "A's Pitch moved", juce::String (pitchStart, 3) + " to " + juce::String (a.value (pitch), 3));
    check (near (b.value (pitch), a.value (pitch), 1.0e-4f), "B's Pitch ends where A's does",
           juce::String (a.value (pitch), 4) + " and " + juce::String (b.value (pitch), 4));
    const auto pitchAfter = listen (b, logDir, "b-after-pitch");
    const double expectedHz = 110.0 * std::pow (8.0, (double) a.value (pitch));
    check (std::abs (pitchAfter.hz - expectedHz) < expectedHz * 0.03 && std::abs (pitchAfter.hz - pitchBefore.hz) > pitchBefore.hz * 0.5,
           "B's sound changes pitch",
           juce::String (pitchBefore.hz, 0) + " Hz to " + juce::String (pitchAfter.hz, 0) + " Hz, expected "
           + juce::String (expectedHz, 0));
    printRates ("plugin sweep, 1 s, throttle 33 ms", s0, s1);

    //==========================================================================
    print ("\n3. Message rates during a fast sweep");
    for (double interval : { 0.0, 16.0, 33.0, 50.0 })
    {
        auto o = a.session.getSync().options();
        o.flushIntervalMs = interval;
        a.session.getSync().setOptions (o);
        const auto r0 = snap (a);
        hostSweep (a, level, 0.1f, 0.9f, 200, 5);   // 200 values in about a second
        pump (200);
        const auto r1 = snap (a);
        printRates ("host sweep, 200 values, throttle " + juce::String (interval, 0) + " ms", r0, r1);
        check (near (b.value (level), 0.9f, 1.0e-4f), "B ends on the sweep's last value with throttle " + juce::String (interval, 0) + " ms");
    }
    {
        auto o = a.session.getSync().options();
        o.flushIntervalMs = 33.0;
        a.session.getSync().setOptions (o);
    }

    //==========================================================================
    print ("\n4. A plugin that echoes what the host sets");
    auto* echo = b.session.findParameterByName ("Test: echo host changes");
    check (echo != nullptr, "the test synth has its echo control");
    if (echo != nullptr)
        echo->setValue (1.0f);
    pump (100);

    for (double window : { 250.0, 0.0 })
    {
        for (auto* m : { &a, &b })
        {
            auto o = m->session.getSync().options();
            o.echoWindowMs = window;
            m->session.getSync().setOptions (o);
        }

        const auto e0 = snap (b);
        hostSweep (a, level, 0.123f, 0.777f, 100, 5);
        pump (1000);
        const auto e1 = snap (b);
        const auto sentByB = e1.sync.sentChanges - e0.sync.sentChanges;
        const auto echoes = (e1.sync.echoes - e0.sync.echoes) + (e1.sync.echoesAltered - e0.sync.echoesAltered)
                          + (e1.sync.duplicates - e0.sync.duplicates);
        print ("ECHO window " + juce::String (window, 0) + " ms: B's plugin reported " + juce::String ((juce::int64) (e1.session.trackedEvents - e0.session.trackedEvents))
               + " values back; " + juce::String ((juce::int64) echoes) + " recognised as echoes; B sent "
               + juce::String ((juce::int64) sentByB) + " changes back");

        // B's plugin rounds A's last value (0.777 to 0.78). Once A has let go, that
        // is a real difference, and is sent once so everyone settles on 0.78.
        if (window > 0.0)
            check (sentByB <= 1, "with the echo window, B sends back at most the plugin's rounding of the last value",
                   juce::String ((juce::int64) sentByB) + " sent");

        const bool settled = waitFor ([&] { return near (a.value (level), b.value (level), 0.006f); }, 3000);
        check (settled, "A and B settle on the same Level, within the plugin's rounding",
               juce::String (a.value (level), 4) + " and " + juce::String (b.value (level), 4));
    }

    if (echo != nullptr)
        echo->setValue (0.0f);
    for (auto* m : { &a, &b })
    {
        auto o = m->session.getSync().options();
        o.echoWindowMs = 250.0;
        m->session.getSync().setOptions (o);
    }
    pump (300);

    //==========================================================================
    print ("\n5. Both people move the same control");
    for (int round = 0; round < 2; ++round)
    {
        auto& first = round == 0 ? b : a;    // lets go first
        auto& last  = round == 0 ? a : b;    // lets go last, and wins
        const float lastValue = round == 0 ? 0.35f : 0.65f;

        a.session.hostGestureBegin (level);
        b.session.hostGestureBegin (level);

        for (int i = 0; i < 10; ++i)
        {
            a.session.hostSet (level, 0.2f + 0.01f * (float) i);
            b.session.hostSet (level, 0.7f - 0.01f * (float) i);
            pump (20);
        }

        check (a.value (level) < 0.4f && b.value (level) > 0.5f, "while both hold it, each hears their own value",
               juce::String (a.value (level), 3) + " and " + juce::String (b.value (level), 3));
        check (a.session.getSync().heldBy (level.toStdString()) == "member-b"
               && b.session.getSync().heldBy (level.toStdString()) == "member-a", "each sees the other holding it");

        first.session.hostGestureEnd (level);
        pump (100);
        last.session.hostSet (level, lastValue);
        last.session.hostGestureEnd (level);
        waitFor ([&] { return near (a.value (level), lastValue, 1.0e-4f) && near (b.value (level), lastValue, 1.0e-4f); }, 2000);
        check (near (a.value (level), lastValue, 1.0e-4f) && near (b.value (level), lastValue, 1.0e-4f),
               juce::String ("the last to let go wins (") + (round == 0 ? "A" : "B") + ")",
               juce::String (a.value (level), 4) + " and " + juce::String (b.value (level), 4));
    }

    //==========================================================================
    print ("\n6. Automation is played on each side, never sent");
    a.session.addSweepAutomation (pitch, 2.0);
    b.session.addSweepAutomation (pitch, 2.0);
    pump (100);
    {
        const auto t0 = snap (a), u0 = snap (b);
        float lo[2] { 1.0f, 1.0f }, hi[2] { 0.0f, 0.0f };

        // Both play their own copy of the curve, through the engine as it would on a sound card.
        for (auto* m : { &a, &b })
        {
            auto& transport = m->session.getEdit().getTransport();
            transport.setPosition (te::TimePosition());
            transport.play (false);
        }

        const auto end = juce::Time::getMillisecondCounterHiRes() + 2300.0;

        while (juce::Time::getMillisecondCounterHiRes() < end)
        {
            pump (10);
            int i = 0;
            for (auto* m : { &a, &b })
            {
                lo[i] = std::min (lo[i], m->value (pitch));
                hi[i] = std::max (hi[i], m->value (pitch));
                ++i;
            }
        }

        for (auto* m : { &a, &b })
            m->session.getEdit().getTransport().stop (false, false);

        pump (200);
        const auto t1 = snap (a), u1 = snap (b);
        check (hi[0] - lo[0] > 0.5f && hi[1] - lo[1] > 0.5f, "the curve moves both plugins",
               "A " + juce::String (lo[0], 2) + " to " + juce::String (hi[0], 2) + ", B " + juce::String (lo[1], 2) + " to " + juce::String (hi[1], 2));
        check (t1.sync.sentChanges == t0.sync.sentChanges && u1.sync.sentChanges == u0.sync.sentChanges, "and neither sends anything",
               juce::String ((juce::int64) (t1.sync.sentChanges - t0.sync.sentChanges)) + " and "
               + juce::String ((juce::int64) (u1.sync.sentChanges - u0.sync.sentChanges)) + " changes sent, "
               + juce::String ((juce::int64) (t1.session.trackedEvents - t0.session.trackedEvents)) + " reached A's sync");
        a.session.findParam (pitch)->ap->getCurve().clear (nullptr);
        b.session.findParam (pitch)->ap->getCurve().clear (nullptr);
    }

    //==========================================================================
    print ("\n7. A third member with version 2 of the plugin");
    Member c (engine, "member-c", logC);
    c.session.setEventLog (logDir.getChildFile ("events-c.csv"));
    check (c.session.loadPlugin (v2, error), "C loads the test synth version 2", error);
    c.openWindowOnLinux();
    c.session.getEdit().getTransport().ensureContextAllocated();
    check (c.link.join ("127.0.0.1", port), "C connects to A");
    waitFor ([&] { return a.link.numConnections() == 2; }, 2000);
    c.session.sendHello();
    check (waitFor ([&] { return c.session.getPeers().size() == 2 && a.session.getPeers().size() == 2
                                 && b.session.getPeers().size() == 2; }, 3000), "everyone has everyone's hello");

    const auto& diff = a.session.getMismatches().at ("member-c");
    print ("  A's warnings about C:\n    " + diff.joinIntoString ("\n    "));
    check (diff.joinIntoString (" ").contains ("different version"), "A warns that C has another version");
    check (diff.joinIntoString (" ").contains ("\"Drive\" only for member-c"), "A warns that Drive exists only for C");
    check (logA.joinIntoString ("\n").contains ("WARNING member-a: parameter lists differ from member-c"), "the warning is logged");

    const auto cLevel = c.idOf ("Level");
    check (cLevel == level, "Level has the same ID in both versions", level + " and " + cLevel);
    check (c.session.findParam (cLevel)->index != a.session.findParam (level)->index, "but a different index",
           juce::String (a.session.findParam (level)->index) + " and " + juce::String (c.session.findParam (cLevel)->index));

    hostSweep (a, level, 0.3f, 0.6f, 10, 10);
    waitFor ([&] { return near (b.value (level), 0.6f, 1.0e-4f) && near (c.value (level), 0.6f, 1.0e-4f); }, 2000);
    check (near (b.value (level), 0.6f, 1.0e-4f) && near (c.value (level), 0.6f, 1.0e-4f),
           "A's Level reaches B and C, matched by ID", juce::String (b.value (level), 4) + " and " + juce::String (c.value (level), 4));

    hostSweep (c, c.idOf ("Drive"), 0.0f, 0.5f, 5, 10);
    pump (300);
    check (a.session.getStats().unknownRemoteParams > 0, "C's Drive, which A lacks, is ignored by A",
           juce::String ((juce::int64) a.session.getStats().unknownRemoteParams) + " ignored");
    check (near (b.value (level), 0.6f, 1.0e-4f), "and nothing else moved");

    //==========================================================================
    print ("\nTotals for A: " + juce::String ((juce::int64) a.session.getSync().stats().sentChanges) + " changes sent in "
           + juce::String ((juce::int64) a.session.getSync().stats().sentBatches) + " messages; "
           + juce::String ((juce::int64) a.session.getStats().remoteSetEchoes) + " callbacks from setting remote values swallowed");

    c.link.close();
    b.link.close();
    a.link.close();
    pump (100);
    soundCard.stopThread (2000);

    print (failures == 0 ? "\nPASS all checks" : "\nFAIL " + juce::String (failures) + " checks");
    return failures == 0 ? 0 : 1;
}
} // namespace

namespace spike
{
int runSelfTest (const juce::StringArray& args)
{
    juce::File v1, v2, logDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("plugin-params-selftest");

    for (int i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--v1")            v1 = juce::File (args[i + 1]);
        else if (args[i] == "--v2")       v2 = juce::File (args[i + 1]);
        else if (args[i] == "--log-dir")  logDir = juce::File (args[i + 1]);
    }

    if (! v1.exists() || ! v2.exists())
    {
        print ("usage: plugin_params_spike --self-test --v1 <test synth 1.vst3> --v2 <test synth 2.vst3> [--log-dir <folder>]");
        return 2;
    }

    print ("Logs and renders in " + logDir.getFullPathName());
    return run (v1, v2, logDir);
}
} // namespace spike
