// What the scanner needs to know about the machine it runs on. Spike code (issue #14).

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace scan {

namespace fs = std::filesystem;

std::string osName();     // "windows", "macos" or "linux"
std::string osVersion();  // e.g. "10.0.26100", "13.7.2", a kernel release
std::string archName();   // the scanner's own architecture: "x86_64", "arm64", ...

// The folder names a VST3 bundle uses for binaries this scanner can load, in order
// of preference, such as "x86_64-win" or "arm64-linux". Empty on macOS, where the
// bundle API finds the binary.
std::vector<std::string> bundleArchitectureFolders();

// The usual VST3 folders for this system, the ones that exist.
std::vector<fs::path> defaultVst3Folders();

fs::path homeFolder();
fs::path currentExecutable();

// Replaces the home folder at the start of a path, or anywhere in a log line,
// with "~", so an inventory shared with the other maintainer carries no user name.
std::string withoutHome (const std::string& text);

// The command line as UTF-8. On Windows, argv is in the local code page and
// loses characters outside it, so this reads the wide command line instead.
std::vector<std::string> commandLineUtf8 (int argc, char** argv);

fs::path pathFromUtf8 (const std::string& s);

// Turns a path into UTF-8 text with forward slashes.
std::string displayPath (const fs::path& p);

} // namespace scan
