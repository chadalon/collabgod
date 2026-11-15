// Finding VST3 bundles under the chosen folders. Spike code (issue #14).
// Nothing here loads a plugin.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace scan {

struct FoundBundle
{
    std::size_t root = 0;               // index into the roots given
    std::filesystem::path path;         // absolute
    std::string relative;               // relative to its root, forward slashes
    bool singleFile = false;            // the older Windows layout: the .vst3 is the DLL
};

struct Discovery
{
    std::vector<FoundBundle> bundles;   // sorted by root, then relative path
    std::vector<std::string> problems;  // unreadable folders and the like, home folder removed
};

// Walks each root. A folder or file whose name ends in ".vst3" (any case) is a
// bundle, and the walk does not go inside it. Folder links are followed once;
// a bundle reachable from two roots, or twice through links, is listed once.
Discovery discoverBundles (const std::vector<std::filesystem::path>& roots);

// Reads Contents/Resources/moduleinfo.json from a bundle folder, if there is one.
// This is what a host can know without loading the plugin.
bool readModuleInfo (const std::filesystem::path& bundle, std::string& text);

} // namespace scan
