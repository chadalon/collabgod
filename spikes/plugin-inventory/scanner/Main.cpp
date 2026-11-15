// plugin_scan: an out-of-process VST3 inventory scanner. Spike for issue #14.
//
//   plugin_scan scan [--out FILE] [--label NAME] [--timeout SECONDS] [FOLDER...]
//   plugin_scan probe BUNDLE --out FILE
//   plugin_scan folders
//
// `scan` never loads a plugin itself. It finds bundles, then runs `probe` on
// each one in a child process with a time limit, and writes one inventory JSON.
// See README.md.

#include "ChildProcess.h"
#include "Discover.h"
#include "Json.h"
#include "Platform.h"
#include "Probe.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #include <process.h>
  #define getpid _getpid
#else
  #include <unistd.h>
#endif

namespace {

using namespace scan;

constexpr const char* scannerVersion = "0.1.0";
constexpr std::size_t maxModuleInfoBytes = 256 * 1024;
constexpr std::size_t logTailBytes = 2048;

int usage()
{
    std::fputs (
        "usage:\n"
        "  plugin_scan scan [--out FILE] [--label NAME] [--timeout SECONDS] [FOLDER...]\n"
        "      Scan VST3 folders (the system's usual ones if none are given) and write\n"
        "      an inventory. Each plugin is loaded in its own child process.\n"
        "  plugin_scan probe BUNDLE --out FILE\n"
        "      Load one bundle in this process and write what its factory reports.\n"
        "      Used by scan; run it by hand to debug one plugin.\n"
        "  plugin_scan folders\n"
        "      Print the default VST3 folders on this system.\n",
        stderr);
    return 64;
}

std::string utcNow()
{
    const std::time_t t = std::time (nullptr);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s (&tm, &t);
#else
    gmtime_r (&t, &tm);
#endif
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string readFile (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    return std::string (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
}

std::string tail (const std::string& s, std::size_t n)
{
    if (s.size() <= n)
        return s;
    // Step forward to a character boundary so the cut does not split UTF-8.
    std::size_t start = s.size() - n;
    while (start < s.size() && (static_cast<unsigned char> (s[start]) & 0xC0) == 0x80)
        ++start;
    return "..." + s.substr (start);
}

const char* endingName (ChildResult::Ending e)
{
    switch (e)
    {
        case ChildResult::Ending::exited:        return "exited";
        case ChildResult::Ending::crashed:       return "crashed";
        case ChildResult::Ending::timedOut:      return "timed-out";
        case ChildResult::Ending::couldNotStart: return "could-not-start";
    }
    return "unknown";
}

// Pulls "result" out of a probe's JSON for the progress line, without a parser.
std::string probeResult (const std::string& json)
{
    const std::string key = "\"result\":\"";
    const auto pos = json.find (key);
    if (pos == std::string::npos)
        return "?";
    const auto end = json.find ('"', pos + key.size());
    return json.substr (pos + key.size(), end - pos - key.size());
}

int runScan (const std::vector<std::string>& args)
{
    fs::path out = "plugin-inventory.json";
    std::string label;
    std::chrono::milliseconds timeout (30'000);
    std::vector<fs::path> roots;

    const auto argc = args.size();
    for (std::size_t i = 2; i < argc; ++i)
    {
        const std::string& a = args[i];
        if (a == "--out" && i + 1 < argc)          out = pathFromUtf8 (args[++i]);
        else if (a == "--label" && i + 1 < argc)   label = args[++i];
        else if (a == "--timeout" && i + 1 < argc) timeout = std::chrono::milliseconds (static_cast<long long> (std::stod (args[++i]) * 1000));
        else if (a.rfind ("--", 0) == 0)           return usage();
        else                                       roots.push_back (pathFromUtf8 (a));
    }

    if (roots.empty())
        roots = defaultVst3Folders();
    if (roots.empty())
    {
        std::fputs ("No folders given and none of the usual VST3 folders exist.\n", stderr);
        return 1;
    }

    const auto self = currentExecutable();
    std::error_code ec;
    const auto work = fs::temp_directory_path (ec) / ("collabgod-plugin-scan-" + std::to_string (getpid()));
    fs::create_directories (work, ec);
    if (ec)
    {
        std::fprintf (stderr, "Cannot create a working folder: %s\n", ec.message().c_str());
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto discovery = discoverBundles (roots);
    const auto& bundles = discovery.bundles;
    std::fprintf (stderr, "Found %zu VST3 bundles in %zu folders. Time limit per plugin: %.0f s.\n",
                  bundles.size(), roots.size(), static_cast<double> (timeout.count()) / 1000.0);

    JsonWriter j;
    j.beginObject()
        .field ("schema", "collabgod.plugin-inventory/1")
        .field ("scanner", scannerVersion)
        .field ("label", label)
        .key ("machine").beginObject()
            .field ("os", osName())
            .field ("osVersion", osVersion())
            .field ("arch", archName())
        .endObject()
        .field ("scannedAt", utcNow())
        .field ("timeoutMs", static_cast<std::int64_t> (timeout.count()));

    j.key ("roots").beginArray();
    for (std::size_t r = 0; r < roots.size(); ++r)
    {
        std::int64_t count = 0;
        for (const auto& b : bundles)
            count += b.root == r ? 1 : 0;
        j.beginObject()
            .field ("path", withoutHome (displayPath (roots[r])))
            .field ("bundles", count)
            .endObject();
    }
    j.endArray();

    j.key ("problems").beginArray();
    for (const auto& p : discovery.problems)
        j.value (p);
    j.endArray();

    j.key ("bundles").beginArray();
    for (std::size_t i = 0; i < bundles.size(); ++i)
    {
        const auto& b = bundles[i];
        const auto probeOut = work / (std::to_string (i) + ".json");
        const auto probeLog = work / (std::to_string (i) + ".log");

        const auto child = runChild (self, { "probe", b.path, "--out", probeOut }, probeLog, timeout);

        const bool written = fs::is_regular_file (probeOut, ec);
        const std::string probeJson = written ? readFile (probeOut) : std::string();
        const bool probeValid = probeJson.size() >= 2 && probeJson.front() == '{' && probeJson.back() == '}';
        const bool clean = child.ending == ChildResult::Ending::exited && child.exitCode == 0 && probeValid;

        j.beginObject()
            .field ("root", static_cast<std::int64_t> (b.root))
            .field ("path", b.relative)
            .field ("layout", b.singleFile ? "single-file" : "bundle")
            .field ("ending", endingName (child.ending))
            .field ("elapsedMs", static_cast<std::int64_t> (child.elapsedMs + 0.5))
            .field ("probeWritten", probeValid);
        if (child.ending == ChildResult::Ending::exited)
            j.field ("exitCode", static_cast<std::int64_t> (child.exitCode));
        if (! child.crashDetail.empty())
            j.field ("crash", child.crashDetail);

        std::string moduleInfo;
        j.key ("moduleInfo");
        if (! b.singleFile && readModuleInfo (b.path, moduleInfo))
            j.value (moduleInfo.size() > maxModuleInfoBytes ? std::string ("(too large to include)") : moduleInfo);
        else
            j.null();

        j.key ("probe");
        if (probeValid) j.raw (probeJson);
        else            j.null();

        // The child's own output, kept only when something went wrong.
        const std::string result = probeValid ? probeResult (probeJson) : std::string ("none");
        if (! clean || result != "ok")
        {
            const auto log = readFile (probeLog);
            if (! log.empty())
                j.field ("log", withoutHome (tail (log, logTailBytes)));
        }
        j.endObject();

        std::fprintf (stderr, "[%zu/%zu] %s: %s%s%s, %.0f ms\n", i + 1, bundles.size(), b.relative.c_str(),
                      endingName (child.ending), probeValid ? ", " : "", probeValid ? result.c_str() : "",
                      child.elapsedMs);
    }
    j.endArray();

    const auto totalMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - started).count();
    j.field ("durationMs", static_cast<std::int64_t> (totalMs + 0.5));
    j.endObject();

    fs::remove_all (work, ec);

    std::ofstream f (out, std::ios::binary | std::ios::trunc);
    f << j.str() << '\n';
    if (! f.flush())
    {
        std::fprintf (stderr, "Cannot write %s\n", displayPath (out).c_str());
        return 1;
    }
    std::fprintf (stderr, "Wrote %s in %.1f s.\n", displayPath (out).c_str(), totalMs / 1000.0);
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    const auto args = commandLineUtf8 (argc, argv);
    if (args.size() < 2)
        return usage();

    const std::string& command = args[1];

    if (command == "scan")
        return runScan (args);

    if (command == "probe")
    {
        if (args.size() != 5 || args[3] != "--out")
            return usage();
        return probeBundle (pathFromUtf8 (args[2]), pathFromUtf8 (args[4]));
    }

    if (command == "folders")
    {
        for (const auto& f : defaultVst3Folders())
            std::printf ("%s\n", displayPath (f).c_str());
        return 0;
    }

    return usage();
}
