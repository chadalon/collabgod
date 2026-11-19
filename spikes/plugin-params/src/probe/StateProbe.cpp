// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.
//
//   plugin_params_spike --probe-state <plugin.vst3 or folder> [--out <report.md>] [--try-bad-state]
//
// Measures what syncing each plugin's saved state would cost, without a second
// machine or a person: how big its state is, how long a read and a load take,
// whether it saves the same bytes twice in a row, whether another copy loaded
// with those bytes saves them back unchanged (so members can compare states by
// hash), whether it rewrites its state some time after a load, and which file
// paths its state names. With a folder, each plugin is probed in its own
// process, so one that crashes or hangs is recorded and the rest go on.
// --try-bad-state also loads a truncated state, to see whether the plugin
// refuses it, takes it, or crashes; it is off by default because a plugin may
// crash on it, which only costs that plugin's row.
//
// Writes a Markdown report (by default to Documents/CollabGod Spike Reports/
// plugin-state) to paste into the issue. Needs no audio device.

#include "../selftest/Harness.h"
#include "../selftest/SelfTest.h"

namespace
{
using namespace spike::harness;
using spike::PluginSession;
using spike::StateHost;

juce::File defaultReport()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("CollabGod Spike Reports").getChildFile ("plugin-state")
               .getChildFile ("probe-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".md");
}

juce::String size (std::uint64_t bytes)
{
    if (bytes >= 1024 * 1024)
        return juce::String ((double) bytes / (1024.0 * 1024.0), 2) + " MiB";
    if (bytes >= 1024)
        return juce::String ((double) bytes / 1024.0, 1) + " KiB";
    return juce::String ((juce::int64) bytes) + " B";
}

juce::uint64 zippedSize (const juce::MemoryBlock& b)
{
    juce::MemoryOutputStream out;
    {
        juce::GZIPCompressorOutputStream z (out, 6, juce::GZIPCompressorOutputStream::windowBitsRaw);
        z.write (b.getData(), b.getSize());
    }
    return (juce::uint64) out.getDataSize();
}

juce::String yesNo (bool b) { return b ? "yes" : "**no**"; }

const char* const header =
    "| Plugin | Version | Parameters synced / reported | State (component + controller) | Compressed | Read ms (median, max) "
    "| Load ms | Same bytes twice | Two fresh copies match | Load own state: saves it back | Load into another copy: saves it back "
    "| Changes after a load | Hints while probing | Paths in the state | Truncated state |\n"
    "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";

/** Probes one plugin in this process. Returns a table row and notes. */
juce::String probeOne (const juce::File& file, bool tryBad)
{
    te::Engine engine ("CollabGodPluginStateProbe", nullptr, std::make_unique<HeadlessBehaviour>());
    engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();

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

    juce::StringArray logLines;
    Member a (engine, "probe-a", logLines), b (engine, "probe-b", logLines);
    juce::String error;

    if (! a.session.loadPlugin (file, error) || ! b.session.loadPlugin (file, error))
        return "| " + file.getFileName() + " | | | | | | | | | | | | | | did not load: " + error.replace ("|", "/") + " |\n";

    a.session.getEdit().getTransport().ensureContextAllocated();
    b.session.getEdit().getTransport().ensureContextAllocated();
    soundCard.startThread (juce::Thread::Priority::highest);

    // Some plugins finish loading in the background.
    pump (1500);

    auto& sa = a.session.getState();
    auto& sb = b.session.getState();
    sa.enabled = sb.enabled = false;   // nothing is connected; only the reads are wanted
    const auto& h = a.session.getHello();

    // Reads: size, time, and whether the bytes stay the same with nothing done.
    std::vector<double> readMs;
    std::vector<std::string> hashes;
    spike::SavedState first;

    for (int i = 0; i < 5; ++i)
    {
        auto s = sa.read();
        readMs.push_back (s.readMs);
        hashes.push_back (s.hash);
        if (i == 0)
            first = std::move (s);
        pump (200);
    }

    std::sort (readMs.begin(), readMs.end());
    const bool deterministic = std::all_of (hashes.begin(), hashes.end(), [&] (auto& x) { return x == hashes[0]; });
    const bool sameDefault = sb.read().hash == first.hash;

    // Load its own state back.
    auto* instA = a.session.getInstance();
    auto t0 = PluginSession::nowMs();
    instA->setStateInformation (first.bytes.getData(), (int) first.bytes.getSize());
    const double loadOwnMs = PluginSession::nowMs() - t0;
    pump (50);
    const bool roundTrip = sa.read().hash == first.hash;

    // Change B a little first, so loading A's state into it is a real change: move every parameter.
    for (auto& p : b.session.getParams())
        p.jp->setValueNotifyingHost (p.jp->getValue() < 0.5f ? 0.75f : 0.25f);
    pump (200);
    const auto bBefore = sb.read().hash;

    t0 = PluginSession::nowMs();
    b.session.getInstance()->setStateInformation (first.bytes.getData(), (int) first.bytes.getSize());
    const double loadOtherMs = PluginSession::nowMs() - t0;
    pump (50);
    const auto bAfter = sb.read().hash;
    const bool intoOther = bAfter == first.hash;

    // Does it change its state again by itself after the load?
    pump (2000);
    const auto bLater = sb.read().hash;
    juce::String settles = bLater == bAfter ? "no" : "**yes**, within 2 s";

    // What the parameters say about the load.
    std::map<std::string, float> sent;
    for (auto& [id, v] : a.session.paramValues())
        sent[id] = v;
    const auto report = psync::classifyApply (first.hash, bBefore, bAfter, sent, b.session.paramValues());

    // Paths.
    juce::StringArray paths;
    for (auto& p : StateHost::pathsIn (first.bytes))
        paths.add (juce::String (psync::redactHome (p)).replace ("|", "/"));

    // A truncated state, if asked for.
    // The plugin's own component state cut in half, as a transfer that went
    // wrong might leave it, wrapped as JUCE expects so it reaches the plugin.
    juce::String bad = "not tried";
    if (tryBad)
    {
        auto [component, controller] = StateHost::splitVst3State (first.bytes);

        if (component.getSize() < 2)
        {
            bad = "no component state to cut";
        }
        else
        {
            component.setSize (component.getSize() / 2);
            const auto cut = StateHost::joinVst3State (component, controller);
            const auto before = sa.read().hash;
            instA->setStateInformation (cut.getData(), (int) cut.getSize());
            pump (200);
            const auto after = sa.read().hash;
            bad = after == before ? "refused: state unchanged" : "**taken**: state changed, no error";
        }
    }

    const auto& stats = sa.getStats();
    const auto& statsB = sb.getStats();
    juce::String hints;
    hints << "setDirty " << (juce::int64) (stats.hintSetDirty + statsB.hintSetDirty) << ", restart " << (juce::int64) (stats.hintRestart + statsB.hintRestart);

    juce::String row;
    row << "| " << h.plugin.name << " (" << h.plugin.vendor << ") | " << h.plugin.version << " | "
        << (int) h.params.size() << " / " << a.session.getInstance()->getParameters().size() << " | "
        << size (first.bytes.getSize()) << " (" << size (first.componentBytes) << " + " << size (first.controllerBytes) << ") | "
        << size (zippedSize (first.bytes)) << " | "
        << juce::String (readMs[readMs.size() / 2], 2) << ", " << juce::String (readMs.back(), 2) << " | "
        << juce::String (loadOwnMs, 1) << ", " << juce::String (loadOtherMs, 1) << " | "
        << yesNo (deterministic) << " | " << yesNo (sameDefault) << " | " << yesNo (roundTrip) << " | "
        << (intoOther ? juce::String ("yes") : "**no**: " + juce::String (psync::toString (report.outcome))) << " | "
        << settles << " | " << hints << " | "
        << (paths.isEmpty() ? juce::String ("none found") : juce::String (paths.size()) + ": " + paths.joinIntoString ("; ", 0, 5)) << " | "
        << bad << " |\n";

    soundCard.stopThread (2000);
    return row;
}
} // namespace

namespace spike
{
juce::Array<juce::File> findPlugins (const juce::File& folder)
{
    juce::Array<juce::File> found;

    // A .vst3 is a file on some Windows installs and a folder (bundle) elsewhere; do not look inside bundles.
    std::function<void (const juce::File&, int)> walk = [&] (const juce::File& dir, int depth)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto f = entry.getFile();
            if (f.hasFileExtension ("vst3"))
                found.add (f);
            else if (entry.isDirectory() && depth < 4)
                walk (f, depth + 1);
        }
    };

    walk (folder, 0);
    return found;
}

int runStateProbe (const juce::StringArray& args)
{
    juce::File target, out = defaultReport(), childOut;
    const bool tryBad = args.contains ("--try-bad-state");

    for (int i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--probe-state")   target = juce::File (args[i + 1]);
        else if (args[i] == "--out")      out = juce::File (args[i + 1]);
        else if (args[i] == "--child")    childOut = juce::File (args[i + 1]);
    }

    if (! target.exists())
    {
        print ("usage: plugin_params_spike --probe-state <plugin.vst3 or folder> [--out <report.md>] [--try-bad-state]");
        return 2;
    }

    // One plugin, for the parent process.
    if (childOut != juce::File())
    {
        childOut.replaceWithText (probeOne (target, tryBad));
        return 0;
    }

    const auto plugins = target.hasFileExtension ("vst3") ? juce::Array<juce::File> { target } : findPlugins (target);
    juce::String report;
    report << "## Plugin state probe (" << juce::SystemStats::getOperatingSystemName() << ", "
           << juce::Time::getCurrentTime().toString (true, false) << ")\n\n"
           << "Each plugin loaded twice in one process with a simulated sound card. \"Load own state\" loads the bytes it just saved; "
              "\"Load into another copy\" loads them into a second copy whose parameters were all moved first. "
              "Read and load times are for the message thread. Paths have the home folder replaced with ~.\n\n"
           << header;

    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    for (auto& plugin : plugins)
    {
        print ("Probing " + plugin.getFullPathName());
        const auto tmp = juce::File::createTempFile (".md");
        juce::ChildProcess child;
        juce::StringArray command { exe.getFullPathName(), "--probe-state", plugin.getFullPathName(), "--child", tmp.getFullPathName() };
        if (tryBad)
            command.add ("--try-bad-state");

        juce::String row;

        if (! child.start (command, 0))
        {
            row = "| " + plugin.getFileName() + " | | | | | | | | | | | | | | could not start a probe process |\n";
        }
        else if (! child.waitForProcessToFinish (120000))
        {
            child.kill();
            row = "| " + plugin.getFileName() + " | | | | | | | | | | | | | | **hung** for 2 minutes; killed |\n";
        }
        else if (! tmp.existsAsFile() || tmp.getSize() == 0)
        {
            row = "| " + plugin.getFileName() + " | | | | | | | | | | | | | | **crashed** (exit code "
                + juce::String ((int) child.getExitCode()) + ") |\n";
        }
        else
        {
            row = tmp.loadFileAsString();
        }

        tmp.deleteFile();
        report << row;
        print (row.trimEnd());
    }

    out.getParentDirectory().createDirectory();
    out.replaceWithText (report);
    juce::SystemClipboard::copyTextToClipboard (report);
    print ("\nReport written to " + out.getFullPathName() + " and copied to the clipboard.");
    return 0;
}
} // namespace spike
