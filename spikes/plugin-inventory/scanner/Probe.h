// Loading one VST3 bundle and reading its class factory. Spike code (issue #14).
//
// This is the only code that runs a plugin's own code, and it only ever runs in
// a child process started by `plugin_scan scan`. A plugin that crashes or hangs
// here takes down that child and nothing else.

#pragma once

#include <filesystem>
#include <string>

namespace scan {

// Loads the bundle, reads its factory and classes, and writes one JSON object to
// `out` (through a temporary file and a rename, so the file is either complete
// or absent). Only after the file is in place does it release the factory and
// call the module's exit function, so a crash while unloading is distinguishable
// from a crash while loading.
//
// Returns the process exit code: 0 when the JSON was written, whatever it says.
int probeBundle (const std::filesystem::path& bundle, const std::filesystem::path& out);

// Canonical text form of a VST3 class ID: the four 32-bit words of the ID as
// 32 upper-case hex digits. This is what Steinberg's FUID::toString prints on
// every platform. The raw 16 bytes differ: on Windows they are in COM GUID byte
// order, elsewhere big-endian. Comparing raw bytes across systems is wrong.
std::string canonicalClassId (const char (&tuid)[16]);

} // namespace scan
