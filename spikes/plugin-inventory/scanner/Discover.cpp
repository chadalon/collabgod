// Spike code (issue #14). See Discover.h.

#include "Discover.h"

#include "Platform.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

namespace scan {

namespace {

bool hasVst3Extension (const fs::path& p)
{
    auto ext = displayPath (p.extension());
    std::transform (ext.begin(), ext.end(), ext.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return ext == ".vst3";
}

std::string canonicalKey (const fs::path& p)
{
    std::error_code ec;
    auto c = fs::canonical (p, ec);
    return displayPath (ec ? fs::absolute (p, ec) : c);
}

} // namespace

Discovery discoverBundles (const std::vector<fs::path>& roots)
{
    Discovery d;
    std::set<std::string> seenBundles;

    for (std::size_t rootIndex = 0; rootIndex < roots.size(); ++rootIndex)
    {
        const auto& root = roots[rootIndex];
        std::error_code ec;

        auto add = [&] (const fs::path& path, bool singleFile)
        {
            if (! seenBundles.insert (canonicalKey (path)).second)
                return;
            std::error_code relEc;
            auto rel = fs::relative (path, root, relEc);
            FoundBundle b;
            b.root = rootIndex;
            b.path = fs::absolute (path, relEc);
            b.relative = relEc || rel.empty() || rel == "." ? displayPath (path.filename()) : displayPath (rel);
            b.singleFile = singleFile;
            d.bundles.push_back (std::move (b));
        };

        if (! fs::exists (root, ec))
        {
            d.problems.push_back ("folder does not exist: " + withoutHome (displayPath (root)));
            continue;
        }

        // The root may itself be a bundle.
        if (hasVst3Extension (root))
        {
            add (root, fs::is_regular_file (root, ec));
            continue;
        }

        std::set<std::string> seenFolders { canonicalKey (root) };
        const auto options = fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator it (root, options, ec);
        if (ec)
        {
            d.problems.push_back ("cannot read " + withoutHome (displayPath (root)) + ": " + ec.message());
            continue;
        }

        for (; it != fs::recursive_directory_iterator(); it.increment (ec))
        {
            if (ec)
            {
                d.problems.push_back ("error while walking " + withoutHome (displayPath (root)) + ": " + ec.message());
                ec.clear();
                break;
            }

            const auto& entry = *it;
            std::error_code typeEc;
            const bool isDir = entry.is_directory (typeEc);

            if (hasVst3Extension (entry.path()))
            {
                add (entry.path(), ! isDir && entry.is_regular_file (typeEc));
                if (isDir)
                    it.disable_recursion_pending();
                continue;
            }

            // Follow each folder once, so links that loop back are harmless.
            if (isDir && ! seenFolders.insert (canonicalKey (entry.path())).second)
                it.disable_recursion_pending();
        }
    }

    std::sort (d.bundles.begin(), d.bundles.end(), [] (const FoundBundle& a, const FoundBundle& b)
    {
        return a.root != b.root ? a.root < b.root : a.relative < b.relative;
    });
    return d;
}

bool readModuleInfo (const fs::path& bundle, std::string& text)
{
    const auto file = bundle / "Contents" / "Resources" / "moduleinfo.json";
    std::error_code ec;
    if (! fs::is_regular_file (file, ec))
        return false;
    std::ifstream in (file, std::ios::binary);
    if (! in)
        return false;
    text.assign (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace scan
