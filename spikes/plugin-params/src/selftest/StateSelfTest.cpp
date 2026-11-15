// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.
//
//   plugin_params_spike --self-test-state --v1 <test synth 1.vst3> --v2 <test synth 2.vst3> [--log-dir <folder>]
//
// Three members on this machine, each with its own Tracktion Engine edit and
// its own copy of the test VST3, connected over TCP on 127.0.0.1. Checks that a
// change to the part of the plugin that is not a parameter (its wavetable)
// reaches the others as its saved state and changes their sound; how it is
// found with and without the plugin's setDirty; that a parameter-only change
// transfers nothing; how long large states take; that parameters a plugin
// changed without telling the host are found; that two people changing it at
// once converge; that a noisy or settling plugin does not loop; what happens
// when a state names a file the other machine lacks; and what a member with
// version 2, whose saved format version 1 refuses, sees. Prints STATE lines
// with the numbers. Needs no audio device; on Linux, a display for the plugins'
// windows (their timers need them there).

#include "Harness.h"
#include "SelfTest.h"

namespace
{
using namespace spike::harness;
using spike::PluginSession;
using spike::Detect;

int stateFailures = 0;

void stateCheck (bool ok, const juce::String& what, const juce::String& detail = {})
{
    check (ok, what, detail);
    if (! ok)
        ++stateFailures;
}

juce::String hashOf (Member& m)        { return juce::String (m.session.getState().read().hash).substring (0, 12); }
const spike::StateHostStats& st (Member& m) { return m.session.getState().getStats(); }

/** Sets one of the test plugin's controls, as a host sets any parameter. It
    stands for a person doing something in the plugin's window, so it counts as
    activity here (which the app finds from the mouse), but it is not a hint. */
void setTest (Member& m, const juce::String& name, float normalised)
{
    m.session.getState().localParamChange (PluginSession::nowMs());

    if (auto* p = m.session.findParameterByName (name))
        p->setValueNotifyingHost (normalised);
    else
        stateCheck (false, "the test synth has its control \"" + name + "\"");
}

float testValue (Member& m, const juce::String& name)
{
    auto* p = m.session.findParameterByName (name);
    return p != nullptr ? p->getValue() : -1.0f;
}

/** True when no state is on its way and every member's plugin saves the same
    bytes. Reading a 10 MiB state takes over 100 ms, so while a transfer is
    on its way this only looks at what the hosts last read. */
bool sameState (std::initializer_list<Member*> members)
{
    juce::String first;
    for (auto* m : members)
    {
        if (m->session.getState().isTransferring() || m->session.getState().getSync().isWaiting())
            return false;
        const auto h = m->session.getState().lastRead().bytes.getSize() > 256 * 1024
                         ? juce::String (m->session.getState().getSync().baseline()).substring (0, 12)
                         : hashOf (*m);
        if (first.isEmpty())
            first = h;
        else if (h != first)
            return false;
    }
    return true;
}

double hzOf (Member& m, const juce::String& pitchId)
{
    return 110.0 * std::pow (8.0, (double) m.value (pitchId));
}

juce::String kib (std::uint64_t bytes) { return juce::String ((double) bytes / 1024.0, 1) + " KiB"; }

void printState (const juce::String& label, Member& m)
{
    const auto& s = st (m);
    const auto& y = m.session.getState().getSync().stats();
    print ("STATE " + label + " " + m.session.getMember() + ": reads " + juce::String ((juce::int64) y.reads)
           + " (" + juce::String (y.reads > 0 ? y.readMsTotal / (double) y.reads : 0.0, 3) + " ms on average, " + juce::String (y.readMsMax, 2) + " at most)"
           + ", hints: setDirty " + juce::String ((juce::int64) s.hintSetDirty) + ", restart " + juce::String ((juce::int64) s.hintRestart)
           + ", unchanged reports " + juce::String ((juce::int64) s.hintSameValues) + ", window " + juce::String ((juce::int64) s.hintWindow)
           + "; announced " + juce::String ((juce::int64) s.announcesSent) + ", matched " + juce::String ((juce::int64) y.matched)
           + ", fetched " + juce::String ((juce::int64) y.fetches) + ", applied " + juce::String ((juce::int64) s.applies)
           + ", absorbed while settling " + juce::String ((juce::int64) y.absorbedSettling)
           + ", parameters found silently changed " + juce::String ((juce::int64) y.paramsDiscovered)
           + "; data sent " + kib (s.dataBytesSent) + ", received " + kib (s.dataBytesReceived));
}

int run (const juce::File& v1, const juce::File& v2, const juce::File& logDir)
{
    te::Engine engine ("CollabGodPluginStateSpike", nullptr, std::make_unique<HeadlessBehaviour>());
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
    a.session.setEventLog (logDir.getChildFile ("state-events-a.csv"));
    b.session.setEventLog (logDir.getChildFile ("state-events-b.csv"));

    juce::String error;
    stateCheck (a.session.loadPlugin (v1, error), "A loads the test synth version 1", error);
    stateCheck (b.session.loadPlugin (v1, error), "B loads the test synth version 1", error);

    if (stateFailures > 0)
        return 1;

    a.openWindowOnLinux();
    b.openWindowOnLinux();
    a.session.getEdit().getTransport().ensureContextAllocated();
    b.session.getEdit().getTransport().ensureContextAllocated();
    soundCard.startThread (juce::Thread::Priority::highest);
    pump (300);

    const auto pitch = a.idOf ("Pitch"), level = a.idOf ("Level");
    stateCheck (a.session.getState().isDeterministic() && b.session.getState().isDeterministic(),
                "the test synth saves the same bytes twice in a row");

    const int port = a.link.host (0);
    stateCheck (port > 0 && b.link.join ("127.0.0.1", port), "A hosts and B joins");
    waitFor ([&] { return a.link.numConnections() == 1; }, 2000);
    b.session.sendHello();
    waitFor ([&] { return a.session.getPeers().size() == 1 && b.session.getPeers().size() == 1; }, 2000);
    pump (800);

    //==========================================================================
    print ("\n1. Two untouched copies: nothing is sent");
    stateCheck (sameState ({ &a, &b }), "A and B save the same state", hashOf (a) + " and " + hashOf (b));
    stateCheck (st (a).dataBytesReceived == 0 && st (b).dataBytesReceived == 0, "no state bytes travel");

    //==========================================================================
    print ("\n2. A new wavetable on A, which calls setDirty");
    {
        const double hz = hzOf (a, pitch);
        const auto before = timbre (b, logDir, "b-before-table", hz);
        const auto t0 = PluginSession::nowMs();
        setTest (a, "Test: new wavetable", 1.0f);
        const bool synced = waitFor ([&] { return st (b).applies >= 1 && sameState ({ &a, &b }); }, 5000);
        const auto ms = PluginSession::nowMs() - t0;
        stateCheck (synced, "B ends with A's state", hashOf (a) + " and " + hashOf (b));
        stateCheck (st (a).hintSetDirty > 0, "A's host heard setDirty", juce::String ((juce::int64) st (a).hintSetDirty) + " times");
        const auto ta = timbre (a, logDir, "a-after-table", hz), tb = timbre (b, logDir, "b-after-table", hz);
        stateCheck (timbreDistance (ta, tb) < 0.02, "A and B sound the same", "largest harmonic difference " + juce::String (timbreDistance (ta, tb), 4));
        stateCheck (timbreDistance (before, tb) > 0.05, "and B's sound changed", "largest harmonic difference " + juce::String (timbreDistance (before, tb), 4));
        stateCheck (st (b).outcomes.count ("loaded exactly") == 1, "B's plugin saves exactly what it was given");
        print ("STATE setDirty: synced in " + juce::String (ms, 0) + " ms from the change; " + kib (st (b).dataBytesReceived)
               + " of state data; the transfer took " + juce::String (st (b).lastTransferMs, 0) + " ms from asking");
    }

    //==========================================================================
    print ("\n3. A plugin that does not call setDirty");
    {
        setTest (a, "Test: call setDirty", 0.0f);
        pump (100);
        const auto appliedBefore = st (b).applies;
        setTest (a, "Test: new wavetable", 1.0f);
        pump (2000);
        stateCheck (st (b).applies == appliedBefore && ! sameState ({ &a, &b }), "with hints only, nobody finds the change",
                    "B applied " + juce::String ((juce::int64) (st (b).applies - appliedBefore)));

        // The person lets go of the mouse in the plugin's window.
        const auto t0 = PluginSession::nowMs();
        a.session.getState().windowMouse (true, t0);
        a.session.getState().windowMouse (false, t0 + 1.0);
        const bool found = waitFor ([&] { return sameState ({ &a, &b }); }, 5000);
        stateCheck (found, "the mouse let go in A's window finds it", "in " + juce::String (PluginSession::nowMs() - t0, 0) + " ms");

        // Polling instead.
        a.session.getState().detect = Detect::pollAlways;
        const auto t1 = PluginSession::nowMs();
        setTest (a, "Test: new wavetable", 1.0f);
        const bool polled = waitFor ([&] { return st (b).applies >= appliedBefore + 2 && sameState ({ &a, &b }); }, 5000);
        stateCheck (polled, "polling finds it", "in " + juce::String (PluginSession::nowMs() - t1, 0) + " ms, polling every "
                    + juce::String (a.session.getState().getSync().pollIntervalMs(), 0) + " ms");
        setTest (a, "Test: call setDirty", 1.0f);
    }

    //==========================================================================
    print ("\n4. A parameter change: the state changes, but nothing needs to travel");
    {
        b.session.getState().detect = Detect::pollAlways;
        pump (1500);
        const auto received = st (b).dataBytesReceived;
        const auto matched = b.session.getState().getSync().stats().matched + a.session.getState().getSync().stats().matched;
        hostSweep (a, level, 0.3f, 0.7f, 20, 10);
        pump (2500);
        stateCheck (sameState ({ &a, &b }), "A and B save the same state after the knob moved", hashOf (a) + " and " + hashOf (b));
        stateCheck (st (b).dataBytesReceived == received, "and no state bytes travelled", kib (st (b).dataBytesReceived - received));
        const auto matchedNow = b.session.getState().getSync().stats().matched + a.session.getState().getSync().stats().matched;
        print ("STATE knob: " + juce::String ((juce::int64) (matchedNow - matched)) + " announcements matched by hash, "
               + juce::String ((juce::int64) b.session.getState().getSync().stats().absorbedRemoteParams) + " changes on B taken as A's parameters");
        a.session.getState().detect = Detect::hints;
        b.session.getState().detect = Detect::hints;
    }

    //==========================================================================
    print ("\n5. Large states");
    for (auto [choice, label] : { std::pair<float, const char*> { 2.0f / 3.0f, "1 MiB" }, std::pair<float, const char*> { 1.0f, "8 MiB" } })
    {
        // Paced at 32 KiB a tick (about 6 MB/s), so parameters can be seen overtaking it.
        a.session.getState().bytesPerTick = 32 * 1024;
        const auto appliedBefore = st (b).applies;
        const auto t0 = PluginSession::nowMs();
        setTest (a, "Test: embedded data", choice);
        waitFor ([&] { return a.session.getState().isSending(); }, 5000);
        const auto transferStarted = PluginSession::nowMs();

        // A knob moves while the state is on its way.
        a.session.hostGestureBegin (level);
        a.session.hostSet (level, juce::String (label) == "1 MiB" ? 0.45f : 0.55f);
        a.session.hostGestureEnd (level);
        const float target = a.value (level);
        double levelMs = -1.0, appliedMs = -1.0;
        const bool synced = waitFor ([&]
        {
            const auto now = PluginSession::nowMs() - transferStarted;
            if (levelMs < 0.0 && near (b.value (level), target, 1.0e-4f))
                levelMs = now;
            if (appliedMs < 0.0 && st (b).applies > appliedBefore)
                appliedMs = now;
            return st (b).applies > appliedBefore && sameState ({ &a, &b });
        }, 20000);
        const auto ms = PluginSession::nowMs() - t0;
        pump (1000);   // the plugin's late reports of the values it loaded, and their answers
        stateCheck (synced, juce::String ("B ends with A's ") + label + " state");
        stateCheck (near (b.value (level), target, 1.0e-4f), "and with the Level A set during the transfer",
                    "A " + juce::String (a.value (level), 4) + ", B " + juce::String (b.value (level), 4) + ", set " + juce::String (target, 4)
                    + "; B applied " + juce::String ((juce::int64) (st (b).applies - appliedBefore)) + ", put back "
                    + juce::String ((juce::int64) b.session.getState().getSync().stats().paramsPutBack));
        // Timing on a busy machine varies; the 8 MiB transfer is long enough to check it reliably.
        const juce::String timing = "Level after " + juce::String (levelMs, 0) + " ms, state applied after " + juce::String (appliedMs, 0) + " ms";
        if (juce::String (label) == "8 MiB")
            stateCheck (levelMs >= 0.0 && levelMs < appliedMs, "which reached B before the state did", timing);
        else
            print ("  " + timing);
        print ("STATE " + juce::String (label) + ": raw " + kib (st (b).lastRawSize) + ", compressed " + kib (st (b).lastCompressedSize)
               + ", sent as " + kib (st (a).dataBytesSent) + " of JSON so far; synced " + juce::String (ms, 0) + " ms after the change; transfer "
               + juce::String (st (b).lastTransferMs, 0) + " ms at 32 KiB a tick; apply "
               + juce::String (st (b).applyMsMax, 1) + " ms at most; read on A "
               + juce::String (a.session.getState().lastRead().readMs, 2) + " ms");
    }
    a.session.getState().bytesPerTick = 512 * 1024;
    setTest (a, "Test: embedded data", 0.0f);
    waitFor ([&] { return sameState ({ &a, &b }) && a.session.getState().lastRead().bytes.getSize() < 4096; }, 10000);
    pump (500);

    //==========================================================================
    print ("\n6. A preset that moves parameters without telling the host");
    {
        const float levelBefore = a.value (level);
        setTest (a, "Test: unreported preset", 1.0f);
        pump (100);
        stateCheck (near (a.value (level), levelBefore, 1.0e-6f), "A's host still thinks Level is what it was",
                    juce::String (a.value (level), 3));
        const bool synced = waitFor ([&] { return sameState ({ &a, &b }) && near (a.value (level), b.value (level), 1.0e-5f)
                                                  && ! near (a.value (level), levelBefore, 1.0e-4f); }, 5000);
        stateCheck (synced, "B finds the new Level and Pitch, and A's host learns them from B",
                    "Level A " + juce::String (a.value (level), 3) + ", B " + juce::String (b.value (level), 3) + ", before "
                    + juce::String (levelBefore, 3) + "; Pitch A " + juce::String (a.value (pitch), 3) + ", B " + juce::String (b.value (pitch), 3)
                    + "; states " + hashOf (a) + " and " + hashOf (b)
                    + (a.session.getState().isTransferring() || b.session.getState().isTransferring() ? ", still transferring" : ""));
        stateCheck (b.session.getState().getSync().stats().paramsDiscovered >= 2, "B counted them",
                    juce::String ((juce::int64) b.session.getState().getSync().stats().paramsDiscovered));
        const double hz = hzOf (a, pitch);
        stateCheck (timbreDistance (timbre (a, logDir, "a-preset", hz), timbre (b, logDir, "b-preset", hz)) < 0.02,
                    "A and B sound the same");
    }

    //==========================================================================
    print ("\n7. A and B change it at the same time");
    {
        const auto beforeA = hashOf (a), beforeB = hashOf (b);
        const auto announcedA = st (a).announcesSent, announcedB = st (b).announcesSent;
        setTest (a, "Test: new wavetable", 1.0f);
        setTest (b, "Test: new wavetable", 1.0f);
        stateCheck (waitFor ([&] { return st (a).announcesSent > announcedA && st (b).announcesSent > announcedB; }, 5000),
                    "both announce their change");
        const bool converged = waitFor ([&] { return sameState ({ &a, &b }) && hashOf (a) != beforeA && hashOf (b) != beforeB; }, 8000);
        stateCheck (converged, "they end with the same state", hashOf (a) + " and " + hashOf (b));
        const double hz = hzOf (a, pitch);
        stateCheck (timbreDistance (timbre (a, logDir, "a-both", hz), timbre (b, logDir, "b-both", hz)) < 0.02, "and sound the same");
        print ("STATE both at once: A forced " + juce::String ((juce::int64) a.session.getState().getSync().stats().forced)
               + ", B forced " + juce::String ((juce::int64) b.session.getState().getSync().stats().forced));
    }

    //==========================================================================
    print ("\n8. A plugin whose state changes every time it is saved");
    {
        setTest (b, "Test: noisy state", 1.0f);
        pump (100);
        b.session.getState().detect = Detect::pollAlways;
        const auto announced = st (b).announcesSent, appliedA = st (a).applies;
        pump (4000);
        stateCheck (b.session.getState().getSync().isNoisy(), "B's host marks it noisy and stops polling");
        stateCheck (st (b).announcesSent - announced <= 1 && st (a).applies - appliedA <= 1, "and it does not flood A",
                    juce::String ((juce::int64) (st (b).announcesSent - announced)) + " announced, "
                    + juce::String ((juce::int64) (st (a).applies - appliedA)) + " applied by A");
        setTest (b, "Test: noisy state", 0.0f);
        b.session.getState().detect = Detect::hints;
        b.session.getState().getSync().setNoisy (false);
        pump (300);
    }

    //==========================================================================
    print ("\n9. A plugin that rewrites its state after loading one");
    {
        setTest (b, "Test: settle late", 1.0f);
        b.session.getState().detect = Detect::pollAlways;
        a.session.getState().detect = Detect::pollAlways;
        pump (300);
        const auto appliedA = st (a).applies, appliedB = st (b).applies;
        const auto absorbed = b.session.getState().getSync().stats().absorbedSettling;
        setTest (a, "Test: new wavetable", 1.0f);
        waitFor ([&] { return st (b).applies > appliedB; }, 5000);
        pump (3000);
        stateCheck (st (b).applies == appliedB + 1 && st (a).applies == appliedA, "B applies once, and nothing comes back to A",
                    "B applied " + juce::String ((juce::int64) (st (b).applies - appliedB)) + ", A " + juce::String ((juce::int64) (st (a).applies - appliedA)));
        stateCheck (b.session.getState().getSync().stats().absorbedSettling > absorbed, "B took the rewrite as settling");
        const double hz = hzOf (a, pitch);
        stateCheck (timbreDistance (timbre (a, logDir, "a-settle", hz), timbre (b, logDir, "b-settle", hz)) < 0.02, "and they sound the same");
        setTest (b, "Test: settle late", 0.0f);
        a.session.getState().detect = Detect::hints;
        b.session.getState().detect = Detect::hints;
        pump (300);
    }

    //==========================================================================
    print ("\n10. A state that refers to a file the other machine lacks");
    {
        setTest (a, "Test: use a sample file", 1.0f);

        // Wait until A has announced a state that names the file.
        stateCheck (waitFor ([&]
        {
            const auto s = a.session.getState().read();
            bool names = false;
            for (auto& p : spike::StateHost::pathsIn (s.bytes))
                names = names || juce::String (p).contains ("collabgod-test-synth");
            return names && a.session.getState().getSync().baseline() == s.hash && a.session.getState().getSync().agreedHash() == s.hash;
        }, 5000), "A announces a state that names the file");

        // A's plugin has loaded the file. On B's "machine" it does not exist.
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("collabgod-test-synth");
        for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "sample-*.wav"))
            f.deleteFile();

        stateCheck (waitFor ([&] { return testValue (b, "Test: sample missing") > 0.5f; }, 5000), "B's plugin says the sample is missing");
        stateCheck (testValue (a, "Test: sample missing") < 0.5f, "A's still plays it, from memory");
        stateCheck (st (b).pathsMissingHere >= 1, "B's host found the path in the state and saw the file is not there",
                    logB.joinIntoString ("\n").fromLastOccurrenceOf ("are not on this machine: ", false, false).upToFirstOccurrenceOf ("\n", false, false));
    }

    //==========================================================================
    print ("\n11. A third member with version 2, which saves a newer format");
    Member c (engine, "member-c", logC);
    c.session.setEventLog (logDir.getChildFile ("state-events-c.csv"));
    stateCheck (c.session.loadPlugin (v2, error), "C loads the test synth version 2", error);
    c.openWindowOnLinux();
    c.session.getEdit().getTransport().ensureContextAllocated();
    stateCheck (c.link.join ("127.0.0.1", port), "C connects to A");
    waitFor ([&] { return a.link.numConnections() == 2; }, 2000);
    c.session.sendHello();
    {
        stateCheck (waitFor ([&] { return st (c).applies >= 1; }, 5000), "C is given the agreed state when it joins");
        stateCheck (st (c).outcomes.count ("loaded, saved back differently, parameters match") == 1,
                    "version 2 loads version 1's state and saves it back in its own format, with the same parameters");
        const double hzBefore = hzOf (a, pitch);
        const auto aBefore = timbre (a, logDir, "a-before-refused", hzBefore);

        const auto refusedA = st (a).outcomes.count ("not loaded: the plugin's state did not change") ? st (a).outcomes.at ("not loaded: the plugin's state did not change") : 0;
        setTest (c, "Test: new wavetable", 1.0f);
        stateCheck (waitFor ([&] { return st (a).outcomes.count ("not loaded: the plugin's state did not change") > 0
                                          && st (a).outcomes.at ("not loaded: the plugin's state did not change") > refusedA; }, 5000),
                    "version 1 refuses version 2's state, and A's host says so");
        stateCheck (logA.joinIntoString ("\n").contains ("WARNING member-a: applied state"), "the warning is logged");
        stateCheck (timbreDistance (aBefore, timbre (a, logDir, "a-refused", hzBefore)) < 0.02,
                    "so A keeps its old wavetable while C has a new one: a version difference that breaks state sync");
    }

    print ("");
    for (auto* m : { &a, &b, &c })
        printState ("totals", *m);

    c.link.close();
    b.link.close();
    a.link.close();
    pump (100);
    soundCard.stopThread (2000);

    print (stateFailures == 0 ? "\nPASS all checks" : "\nFAIL " + juce::String (stateFailures) + " checks");
    return stateFailures == 0 ? 0 : 1;
}
} // namespace

namespace spike
{
int runStateSelfTest (const juce::StringArray& args)
{
    juce::File v1, v2, logDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("plugin-state-selftest");

    for (int i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--v1")            v1 = juce::File (args[i + 1]);
        else if (args[i] == "--v2")       v2 = juce::File (args[i + 1]);
        else if (args[i] == "--log-dir")  logDir = juce::File (args[i + 1]);
    }

    if (! v1.exists() || ! v2.exists())
    {
        print ("usage: plugin_params_spike --self-test-state --v1 <test synth 1.vst3> --v2 <test synth 2.vst3> [--log-dir <folder>]");
        return 2;
    }

    print ("Logs and renders in " + logDir.getFullPathName());
    return run (v1, v2, logDir);
}
} // namespace spike
