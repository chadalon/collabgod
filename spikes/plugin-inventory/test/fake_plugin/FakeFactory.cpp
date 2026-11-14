// A fake VST3 module for testing the scanner. Spike code (issue #14).
//
// It has a class factory and nothing else: no processor, no editor. That is all a
// scan reads. Compile definitions choose how it behaves:
//
//   FAKE_MODE        normal | crash_on_load | hang_on_load | crash_on_unload
//                    | null_factory | factory_v1 | no_export
//   FAKE_NAME        class name, e.g. "CollabGod Test Synth"
//   FAKE_VERSION     class version, e.g. "1.0.0"
//   FAKE_CID1..4     the class ID's four 32-bit words
//
// Like a real plugin, it refuses to hand out a factory until its entry function
// (bundleEntry, ModuleEntry) has been called, so a passing scan also shows the
// scanner calls it. On Windows, InitDll is optional and is exported here.

#include "pluginterfaces/base/ipluginbase.h"

#include <chrono>
#include <cstring>
#include <thread>

#define FAKE_MODE_normal          0
#define FAKE_MODE_crash_on_load   1
#define FAKE_MODE_hang_on_load    2
#define FAKE_MODE_crash_on_unload 3
#define FAKE_MODE_null_factory    4
#define FAKE_MODE_factory_v1      5
#define FAKE_MODE_no_export       6

#define FAKE_CONCAT2(a, b) a##b
#define FAKE_CONCAT(a, b) FAKE_CONCAT2 (a, b)
#define FAKE_MODE_VALUE FAKE_CONCAT (FAKE_MODE_, FAKE_MODE)

#if defined(_WIN32)
  #define FAKE_EXPORT extern "C" __declspec(dllexport)
#else
  #define FAKE_EXPORT extern "C" __attribute__ ((visibility ("default")))
#endif

using namespace Steinberg;

namespace {

bool entered = false;

[[noreturn]] void crash()
{
    volatile int* nothing = nullptr;
    *nothing = 42;
    for (;;) {}
}

template <std::size_t N>
void copyText (char8 (&dest)[N], const char* src)
{
    std::strncpy (dest, src, N - 1);
    dest[N - 1] = 0;
}

template <std::size_t N>
void copyText (char16 (&dest)[N], const char* src)
{
    std::size_t i = 0;
    for (; i + 1 < N && src[i] != 0; ++i)
        dest[i] = static_cast<char16> (static_cast<unsigned char> (src[i]));
    dest[i] = 0;
}

struct ClassSpec
{
    const char* category;
    const char* name;
    const char* subCategories;
    uint32 l1, l2, l3, l4;
};

constexpr const char* vendor = "CollabGod Tests";
constexpr const char* version = FAKE_VERSION;
constexpr const char* sdkVersion = "VST 3.8.1";

const ClassSpec classes[] = {
    { "Audio Module Class",         FAKE_NAME,               "Instrument|Synth", FAKE_CID1, FAKE_CID2, FAKE_CID3, FAKE_CID4 },
    { "Component Controller Class", FAKE_NAME " Controller", "",                 FAKE_CID1, FAKE_CID2, FAKE_CID3, (FAKE_CID4 + 1u) },
};

void fillCid (TUID dest, const ClassSpec& c)
{
    const TUID id = INLINE_UID (c.l1, c.l2, c.l3, c.l4);
    std::memcpy (dest, id, sizeof (TUID));
}

class FakeFactory : public IPluginFactory3
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        const bool v1Only = FAKE_MODE_VALUE == FAKE_MODE_factory_v1;
        if (std::memcmp (iid, FUnknown_iid, sizeof (TUID)) == 0
            || std::memcmp (iid, IPluginFactory_iid, sizeof (TUID)) == 0
            || (! v1Only && std::memcmp (iid, IPluginFactory2_iid, sizeof (TUID)) == 0)
            || (! v1Only && std::memcmp (iid, IPluginFactory3_iid, sizeof (TUID)) == 0))
        {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override  { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; } // static instance: never deleted

    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        std::memset (info, 0, sizeof (*info));
        copyText (info->vendor, vendor);
        copyText (info->url, "https://github.com/chadalon/collabgod");
        copyText (info->email, "");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return static_cast<int32> (sizeof (classes) / sizeof (classes[0])); }

    tresult PLUGIN_API getClassInfo (int32 index, PClassInfo* info) override
    {
        if (index < 0 || index >= countClasses()) return kInvalidArgument;
        const auto& c = classes[index];
        std::memset (info, 0, sizeof (*info));
        fillCid (info->cid, c);
        info->cardinality = PClassInfo::kManyInstances;
        copyText (info->category, c.category);
        copyText (info->name, c.name);
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (FIDString, FIDString, void** obj) override
    {
        *obj = nullptr;
        return kNotImplemented;
    }

    tresult PLUGIN_API getClassInfo2 (int32 index, PClassInfo2* info) override
    {
        if (index < 0 || index >= countClasses()) return kInvalidArgument;
        const auto& c = classes[index];
        std::memset (info, 0, sizeof (*info));
        fillCid (info->cid, c);
        info->cardinality = PClassInfo::kManyInstances;
        copyText (info->category, c.category);
        copyText (info->name, c.name);
        copyText (info->subCategories, c.subCategories);
        copyText (info->vendor, vendor);
        copyText (info->version, version);
        copyText (info->sdkVersion, sdkVersion);
        return kResultOk;
    }

    tresult PLUGIN_API getClassInfoUnicode (int32 index, PClassInfoW* info) override
    {
        if (index < 0 || index >= countClasses()) return kInvalidArgument;
        const auto& c = classes[index];
        std::memset (info, 0, sizeof (*info));
        fillCid (info->cid, c);
        info->cardinality = PClassInfo::kManyInstances;
        copyText (info->category, c.category);
        copyText (info->name, c.name);
        copyText (info->subCategories, c.subCategories);
        copyText (info->vendor, vendor);
        copyText (info->version, version);
        copyText (info->sdkVersion, sdkVersion);
        return kResultOk;
    }

    tresult PLUGIN_API setHostContext (FUnknown*) override { return kResultOk; }

private:
    uint32 refs = 1;
};

FakeFactory factory;

void onExit()
{
    if (FAKE_MODE_VALUE == FAKE_MODE_crash_on_unload)
        crash();
}

} // namespace

#if FAKE_MODE_VALUE != FAKE_MODE_no_export
FAKE_EXPORT IPluginFactory* PLUGIN_API GetPluginFactory()
{
    if (FAKE_MODE_VALUE == FAKE_MODE_crash_on_load)
        crash();
    if (FAKE_MODE_VALUE == FAKE_MODE_hang_on_load)
        for (;;) std::this_thread::sleep_for (std::chrono::seconds (1));
    if (FAKE_MODE_VALUE == FAKE_MODE_null_factory || ! entered)
        return nullptr;
    factory.addRef();
    return &factory;
}
#endif

#if defined(_WIN32)
FAKE_EXPORT bool InitDll()  { entered = true; return true; }
FAKE_EXPORT bool ExitDll()  { onExit(); return true; }
#elif defined(__APPLE__)
FAKE_EXPORT bool bundleEntry (void*) { entered = true; return true; }
FAKE_EXPORT bool bundleExit()        { onExit(); return true; }
#else
FAKE_EXPORT bool ModuleEntry (void*) { entered = true; return true; }
FAKE_EXPORT bool ModuleExit()        { onExit(); return true; }
#endif
