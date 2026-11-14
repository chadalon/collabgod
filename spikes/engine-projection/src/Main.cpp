// Spike for issue #11: the TypeScript session model drives Tracktion Engine.
// Throwaway code: product code never includes this.
//
//   engine_projection serve [--simulated-audio]     JSON requests on stdin, replies on stdout (contract/README.md)
//   engine_projection selftest [--out dir]          Checks with a simulated sound card; ctest runs this
//   engine_projection bench [--out dir] [--device]  What an edit costs as the session grows
//
// Each takes --audio-cpus N: how many CPUs Tracktion may use for audio (default: all), and
// --tracktion-wave-nodes: play clips the way Tracktion does by default, with WaveNode, which
// waits on its file cache on the audio thread. Without it, clips are played with proxies off,
// by WaveNodeRealTime, which does not wait (README finding 4).

#include <JuceHeader.h>
#include <iostream>

#include "Host.h"

namespace
{
juce::File outFolder (const juce::StringArray& args, const char* fallback)
{
    const auto index = args.indexOf ("--out");
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    auto folder = index > 0 && index + 1 < args.size() ? cwd.getChildFile (args[index + 1]) : cwd.getChildFile (fallback);
    folder.createDirectory();
    return folder;
}

projection::Options options (const juce::StringArray& args)
{
    projection::Options o;
    const auto index = args.indexOf ("--audio-cpus");
    o.audioCpus = index > 0 && index + 1 < args.size() ? juce::jmax (0, args[index + 1].getIntValue()) : 0;
    o.realTimeNodes = ! args.contains ("--tracktion-wave-nodes");
    return o;
}
} // namespace

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juce;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    const auto command = args[0];

    if (command == "serve")
        return projection::serve (args.contains ("--simulated-audio"), options (args));

    if (command == "selftest")
        return projection::selfTest (outFolder (args, "selftest"), options (args));

    if (command == "bench")
        return projection::bench (outFolder (args, "bench"), args.contains ("--device"), options (args));

    std::cout << "Usage:\n"
                 "  engine_projection serve [--simulated-audio]     Drive the engine with JSON lines on stdin\n"
                 "  engine_projection selftest [--out dir]          Check the engine side with a simulated sound card\n"
                 "  engine_projection bench [--out dir] [--device]  Time edits as the session grows\n"
                 "Options for all three:\n"
                 "  --audio-cpus N      CPUs Tracktion may use for audio (default: all)\n"
                 "  --tracktion-wave-nodes   Play clips with Tracktion's default WaveNode, which waits on its file cache\n"
                 "                           on the audio thread (default: proxies off, WaveNodeRealTime, which does not)\n";
    return command.isEmpty() || command == "--help" ? 0 : 2;
}
