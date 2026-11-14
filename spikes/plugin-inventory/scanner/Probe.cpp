// Spike code (issue #14). See Probe.h.
//
// Reads a VST3 module's class factory with the interfaces from Steinberg's
// vst3_pluginterfaces (MIT). No JUCE here: the question is what the plugins
// themselves report, before any host library interprets it.

#include "Probe.h"

#include "Json.h"
#include "Platform.h"

#include "pluginterfaces/base/ipluginbase.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <objbase.h>
#elif defined(__APPLE__)
  #include <CoreFoundation/CoreFoundation.h>
#else
  #include <dlfcn.h>
#endif

namespace scan {

namespace {

using Steinberg::IPluginFactory;
using Steinberg::IPluginFactory2;
using Steinberg::IPluginFactory3;
using Steinberg::PClassInfo;
using Steinberg::PClassInfo2;
using Steinberg::PClassInfoW;
using Steinberg::PFactoryInfo;

using GetFactoryProc = IPluginFactory* (PLUGIN_API*)();

double millisecondsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// Loading a module, per platform. Each keeps what it needs to call the
// module's exit function later; none unloads the binary, because the process
// exits straight afterwards and some plugins crash when unloaded.
// ---------------------------------------------------------------------------

struct LoadedModule
{
    std::string binary;           // relative to the bundle, for the report
    std::string entryPoint;       // which entry function was called, or "none"
    std::string error;            // set when loading failed
    std::string result = "ok";    // the probe's verdict when something failed
    GetFactoryProc getFactory = nullptr;
    std::vector<std::pair<std::string, std::string>> binaryVersion;

#if defined(_WIN32)
    HMODULE handle = nullptr;
    void callExit()
    {
        using ExitFn = bool (PLUGIN_API*)();
        if (auto fn = reinterpret_cast<ExitFn> (reinterpret_cast<void*> (GetProcAddress (handle, "ExitDll"))))
            fn();
    }
#elif defined(__APPLE__)
    CFBundleRef bundle = nullptr;
    void callExit()
    {
        using ExitFn = bool (*)();
        if (auto fn = reinterpret_cast<ExitFn> (CFBundleGetFunctionPointerForName (bundle, CFSTR ("bundleExit"))))
            fn();
    }
#else
    void* handle = nullptr;
    void callExit()
    {
        using ExitFn = bool (PLUGIN_API*)();
        if (auto fn = reinterpret_cast<ExitFn> (dlsym (handle, "ModuleExit")))
            fn();
    }
#endif
};

#if defined(_WIN32)

std::string lastErrorText (DWORD code)
{
    char* buf = nullptr;
    const auto n = FormatMessageA (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPSTR> (&buf), 0, nullptr);
    std::string text = n > 0 && buf ? std::string (buf, n) : std::string();
    if (buf) LocalFree (buf);
    while (! text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return "error " + std::to_string (code) + (text.empty() ? "" : ": " + text);
}

std::string versionText (DWORD ms, DWORD ls)
{
    return std::to_string (HIWORD (ms)) + "." + std::to_string (LOWORD (ms)) + "."
         + std::to_string (HIWORD (ls)) + "." + std::to_string (LOWORD (ls));
}

// The version resource of the binary: what Explorer shows. Read without loading.
void readFileVersion (const fs::path& binary, LoadedModule& m)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW (binary.c_str(), &ignored);
    if (size == 0)
        return;
    std::vector<char> data (size);
    if (! GetFileVersionInfoW (binary.c_str(), 0, size, data.data()))
        return;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (VerQueryValueW (data.data(), L"\\", reinterpret_cast<void**> (&info), &len) && info && len >= sizeof (*info))
    {
        m.binaryVersion.emplace_back ("fileVersion", versionText (info->dwFileVersionMS, info->dwFileVersionLS));
        m.binaryVersion.emplace_back ("productVersion", versionText (info->dwProductVersionMS, info->dwProductVersionLS));
    }
}

LoadedModule loadModule (const fs::path& bundle)
{
    LoadedModule m;
    fs::path binary;
    std::error_code ec;

    if (fs::is_regular_file (bundle, ec))
    {
        // The older single-file layout: the .vst3 is the DLL itself.
        binary = bundle;
        m.binary = ".";
    }
    else
    {
        for (const auto& arch : bundleArchitectureFolders())
        {
            const auto candidate = bundle / "Contents" / arch / bundle.filename();
            if (fs::is_regular_file (candidate, ec))
            {
                binary = candidate;
                m.binary = "Contents/" + arch + "/" + displayPath (bundle.filename());
                break;
            }
        }
    }

    if (binary.empty())
    {
        m.result = "no-binary";
        m.error = "no binary for this architecture in the bundle";
        return m;
    }

    readFileVersion (binary, m);

    // LOAD_WITH_ALTERED_SEARCH_PATH: the plugin's own folder is searched for the
    // DLLs it depends on, as hosts do.
    m.handle = LoadLibraryExW (binary.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (! m.handle)
    {
        m.result = "load-failed";
        m.error = lastErrorText (GetLastError());
        return m;
    }

    using InitFn = bool (PLUGIN_API*)();
    if (auto init = reinterpret_cast<InitFn> (reinterpret_cast<void*> (GetProcAddress (m.handle, "InitDll"))))
    {
        m.entryPoint = "InitDll";
        if (! init())
        {
            m.result = "entry-failed";
            m.error = "InitDll returned false";
            return m;
        }
    }
    else
    {
        m.entryPoint = "none"; // optional on Windows
    }

    m.getFactory = reinterpret_cast<GetFactoryProc> (reinterpret_cast<void*> (GetProcAddress (m.handle, "GetPluginFactory")));
    return m;
}

#elif defined(__APPLE__)

std::string cfStringText (CFTypeRef value)
{
    if (! value || CFGetTypeID (value) != CFStringGetTypeID())
        return {};
    auto s = static_cast<CFStringRef> (value);
    const auto max = CFStringGetMaximumSizeForEncoding (CFStringGetLength (s), kCFStringEncodingUTF8) + 1;
    std::string buf (static_cast<std::size_t> (max), '\0');
    if (! CFStringGetCString (s, buf.data(), max, kCFStringEncodingUTF8))
        return {};
    buf.resize (std::strlen (buf.c_str()));
    return buf;
}

LoadedModule loadModule (const fs::path& bundle)
{
    LoadedModule m;
    const auto path = bundle.string();
    auto url = CFURLCreateFromFileSystemRepresentation (nullptr, reinterpret_cast<const UInt8*> (path.c_str()),
                                                        static_cast<CFIndex> (path.size()), true);
    if (url)
    {
        m.bundle = CFBundleCreate (nullptr, url);
        CFRelease (url);
    }
    if (! m.bundle)
    {
        m.result = "no-binary";
        m.error = "not a bundle CoreFoundation can open";
        return m;
    }

    // Info.plist: what Finder shows. Read before the executable is loaded.
    if (auto s = cfStringText (CFBundleGetValueForInfoDictionaryKey (m.bundle, CFSTR ("CFBundleShortVersionString"))); ! s.empty())
        m.binaryVersion.emplace_back ("shortVersion", s);
    if (auto s = cfStringText (CFBundleGetValueForInfoDictionaryKey (m.bundle, kCFBundleVersionKey)); ! s.empty())
        m.binaryVersion.emplace_back ("bundleVersion", s);
    if (auto s = cfStringText (CFBundleGetValueForInfoDictionaryKey (m.bundle, kCFBundleIdentifierKey)); ! s.empty())
        m.binaryVersion.emplace_back ("bundleIdentifier", s);

    if (auto exe = CFBundleCopyExecutableURL (m.bundle))
    {
        if (auto rel = CFURLCopyFileSystemPath (exe, kCFURLPOSIXPathStyle))
        {
            m.binary = "Contents/MacOS/" + cfStringText (rel);
            CFRelease (rel);
        }
        CFRelease (exe);
    }
    else
    {
        m.result = "no-binary";
        m.error = "bundle has no executable";
        return m;
    }

    CFErrorRef err = nullptr;
    if (! CFBundleLoadExecutableAndReturnError (m.bundle, &err))
    {
        m.result = "load-failed";
        m.error = "could not load the executable";
        if (err)
        {
            if (auto desc = CFErrorCopyDescription (err))
            {
                m.error = cfStringText (desc);
                CFRelease (desc);
            }
            CFRelease (err);
        }
        return m;
    }

    using EntryFn = bool (*)(CFBundleRef);
    if (auto entry = reinterpret_cast<EntryFn> (CFBundleGetFunctionPointerForName (m.bundle, CFSTR ("bundleEntry"))))
    {
        m.entryPoint = "bundleEntry";
        if (! entry (m.bundle))
        {
            m.result = "entry-failed";
            m.error = "bundleEntry returned false";
            return m;
        }
    }
    else
    {
        m.entryPoint = "none"; // required by the VST3 format; recorded and tried anyway
    }

    m.getFactory = reinterpret_cast<GetFactoryProc> (CFBundleGetFunctionPointerForName (m.bundle, CFSTR ("GetPluginFactory")));
    return m;
}

#else // Linux

LoadedModule loadModule (const fs::path& bundle)
{
    LoadedModule m;
    fs::path binary;
    std::error_code ec;
    auto stem = bundle.stem();
    stem += ".so";

    for (const auto& arch : bundleArchitectureFolders())
    {
        const auto candidate = bundle / "Contents" / arch / stem;
        if (fs::is_regular_file (candidate, ec))
        {
            binary = candidate;
            m.binary = "Contents/" + arch + "/" + displayPath (stem);
            break;
        }
    }

    if (binary.empty())
    {
        m.result = "no-binary";
        m.error = "no binary for this architecture in the bundle";
        return m;
    }

    m.handle = dlopen (binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (! m.handle)
    {
        m.result = "load-failed";
        const char* e = dlerror();
        m.error = e ? withoutHome (e) : "dlopen failed";
        return m;
    }

    using EntryFn = bool (PLUGIN_API*)(void*);
    if (auto entry = reinterpret_cast<EntryFn> (dlsym (m.handle, "ModuleEntry")))
    {
        m.entryPoint = "ModuleEntry";
        if (! entry (m.handle))
        {
            m.result = "entry-failed";
            m.error = "ModuleEntry returned false";
            return m;
        }
    }
    else
    {
        m.entryPoint = "none";
    }

    m.getFactory = reinterpret_cast<GetFactoryProc> (dlsym (m.handle, "GetPluginFactory"));
    return m;
}

#endif

// ---------------------------------------------------------------------------
// Reading the factory.
// ---------------------------------------------------------------------------

struct ClassRecord
{
    std::string cid, category, name, vendor, version, sdkVersion, subCategories, infoLevel;
    std::int64_t classFlags = 0, cardinality = 0;
};

template <typename Interface>
Interface* queryFactory (IPluginFactory* f, const Steinberg::TUID iid)
{
    void* obj = nullptr;
    if (f->queryInterface (iid, &obj) == Steinberg::kResultOk && obj)
        return static_cast<Interface*> (obj);
    return nullptr;
}

std::vector<ClassRecord> readClasses (IPluginFactory* f, IPluginFactory2* f2, IPluginFactory3* f3)
{
    std::vector<ClassRecord> classes;
    const auto count = f->countClasses();

    for (Steinberg::int32 i = 0; i < count; ++i)
    {
        ClassRecord r;

        // The richest form the factory offers. PClassInfoW (factory 3) carries the
        // name, vendor and version as UTF-16; PClassInfo2 as 8-bit text.
        PClassInfoW w {};
        PClassInfo2 two {};
        PClassInfo one {};
        if (f3 && f3->getClassInfoUnicode (i, &w) == Steinberg::kResultOk)
        {
            r.infoLevel = "unicode";
            r.cid = canonicalClassId (w.cid);
            r.category = fixedField (w.category, sizeof (w.category));
            r.name = utf16Field (w.name, PClassInfo::kNameSize);
            r.vendor = utf16Field (w.vendor, PClassInfoW::kVendorSize);
            r.version = utf16Field (w.version, PClassInfoW::kVersionSize);
            r.sdkVersion = utf16Field (w.sdkVersion, PClassInfoW::kVersionSize);
            r.subCategories = fixedField (w.subCategories, sizeof (w.subCategories));
            r.classFlags = w.classFlags;
            r.cardinality = w.cardinality;
        }
        else if (f2 && f2->getClassInfo2 (i, &two) == Steinberg::kResultOk)
        {
            r.infoLevel = "v2";
            r.cid = canonicalClassId (two.cid);
            r.category = fixedField (two.category, sizeof (two.category));
            r.name = fixedField (two.name, sizeof (two.name));
            r.vendor = fixedField (two.vendor, sizeof (two.vendor));
            r.version = fixedField (two.version, sizeof (two.version));
            r.sdkVersion = fixedField (two.sdkVersion, sizeof (two.sdkVersion));
            r.subCategories = fixedField (two.subCategories, sizeof (two.subCategories));
            r.classFlags = two.classFlags;
            r.cardinality = two.cardinality;
        }
        else if (f->getClassInfo (i, &one) == Steinberg::kResultOk)
        {
            r.infoLevel = "v1"; // no vendor or version per class
            r.cid = canonicalClassId (one.cid);
            r.category = fixedField (one.category, sizeof (one.category));
            r.name = fixedField (one.name, sizeof (one.name));
            r.cardinality = one.cardinality;
        }
        else
        {
            r.infoLevel = "unreadable";
        }
        classes.push_back (std::move (r));
    }
    return classes;
}

bool writeAtomically (const fs::path& out, const std::string& text)
{
    auto tmp = out;
    tmp += ".tmp";
    {
        std::ofstream f (tmp, std::ios::binary | std::ios::trunc);
        if (! f)
            return false;
        f << text;
        if (! f.flush())
            return false;
    }
    std::error_code ec;
    fs::rename (tmp, out, ec);
    return ! ec;
}

} // namespace

std::string canonicalClassId (const char (&tuid)[16])
{
    const auto b = [&] (int i) { return static_cast<std::uint32_t> (static_cast<unsigned char> (tuid[i])); };
    auto word = [] (std::uint32_t a, std::uint32_t b2, std::uint32_t c, std::uint32_t d) { return (a << 24) | (b2 << 16) | (c << 8) | d; };

#if COM_COMPATIBLE
    // GUID layout: Data1 little-endian, Data2 and Data3 little-endian, Data4 as bytes.
    // Mirrors FUID::getLong1..4 in Steinberg's funknown.cpp.
    const std::uint32_t l1 = word (b (3), b (2), b (1), b (0));
    const std::uint32_t l2 = word (b (5), b (4), b (7), b (6));
#else
    const std::uint32_t l1 = word (b (0), b (1), b (2), b (3));
    const std::uint32_t l2 = word (b (4), b (5), b (6), b (7));
#endif
    const std::uint32_t l3 = word (b (8), b (9), b (10), b (11));
    const std::uint32_t l4 = word (b (12), b (13), b (14), b (15));

    char buf[33];
    std::snprintf (buf, sizeof (buf), "%08X%08X%08X%08X", l1, l2, l3, l4);
    return buf;
}

int probeBundle (const fs::path& bundle, const fs::path& out)
{
#if defined(_WIN32)
    // No "missing DLL" or crash dialogs: on a scan they would hang until the timeout.
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Hosts load plugins on a COM single-threaded apartment thread; some plugins assume it.
    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
#endif

    const auto start = std::chrono::steady_clock::now();
    JsonWriter j;
    j.beginObject();

    LoadedModule m = loadModule (bundle);
    IPluginFactory* factory = nullptr;
    IPluginFactory2* f2 = nullptr;
    IPluginFactory3* f3 = nullptr;
    std::vector<ClassRecord> classes;
    PFactoryInfo info {};
    bool haveInfo = false;

    try
    {
        if (m.result == "ok" && ! m.getFactory)
        {
            m.result = "no-factory-export";
            m.error = "the module does not export GetPluginFactory";
        }
        else if (m.result == "ok")
        {
            factory = m.getFactory();
            if (! factory)
            {
                m.result = "factory-null";
                m.error = "GetPluginFactory returned null";
            }
            else
            {
                haveInfo = factory->getFactoryInfo (&info) == Steinberg::kResultOk;
                f2 = queryFactory<IPluginFactory2> (factory, Steinberg::IPluginFactory2_iid);
                f3 = queryFactory<IPluginFactory3> (factory, Steinberg::IPluginFactory3_iid);
                classes = readClasses (factory, f2, f3);
            }
        }
    }
    catch (...)
    {
        m.result = "threw";
        m.error = "the plugin threw a C++ exception while its factory was read";
    }

    const auto loadMs = millisecondsSince (start);

    j.field ("result", m.result);
    if (! m.error.empty())
        j.field ("error", withoutHome (m.error));
    j.field ("binary", m.binary);
    j.field ("entryPoint", m.entryPoint.empty() ? std::string ("not reached") : m.entryPoint);
    j.field ("loadMs", static_cast<std::int64_t> (loadMs + 0.5));
    j.field ("factoryInterface", static_cast<std::int64_t> (f3 ? 3 : f2 ? 2 : factory ? 1 : 0));

    j.key ("binaryVersion");
    if (m.binaryVersion.empty())
        j.null();
    else
    {
        j.beginObject();
        for (const auto& [k, v] : m.binaryVersion)
            j.field (k, v);
        j.endObject();
    }

    j.key ("factory");
    if (haveInfo)
    {
        j.beginObject()
            .field ("vendor", fixedField (info.vendor, sizeof (info.vendor)))
            .field ("url", fixedField (info.url, sizeof (info.url)))
            .field ("email", fixedField (info.email, sizeof (info.email)))
            .field ("flags", static_cast<std::int64_t> (info.flags))
            .endObject();
    }
    else
    {
        j.null();
    }

    j.key ("classes").beginArray();
    for (const auto& c : classes)
    {
        j.beginObject()
            .field ("cid", c.cid)
            .field ("category", c.category)
            .field ("name", c.name)
            .field ("vendor", c.vendor)
            .field ("version", c.version)
            .field ("sdkVersion", c.sdkVersion)
            .field ("subCategories", c.subCategories)
            .field ("classFlags", c.classFlags)
            .field ("cardinality", c.cardinality)
            .field ("infoLevel", c.infoLevel)
            .endObject();
    }
    j.endArray();
    j.endObject();

    if (! writeAtomically (out, j.str()))
    {
        std::fprintf (stderr, "probe: could not write %s\n", displayPath (out).c_str());
        return 2;
    }

    // Only now let go of the plugin. If this crashes or hangs, the scanner sees
    // a complete result and a bad exit, and reports a problem on unload.
    if (f3) f3->release();
    if (f2) f2->release();
    if (factory) factory->release();
    if (m.result == "ok" || m.result == "factory-null" || m.result == "no-factory-export")
        m.callExit();

    return 0;
}

} // namespace scan
