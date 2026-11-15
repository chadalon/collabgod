// Spike code (issue #14). See Platform.h.

#include "Platform.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shellapi.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <sys/sysctl.h>
  #include <pwd.h>
  #include <unistd.h>
#else
  #include <sys/utsname.h>
  #include <pwd.h>
  #include <unistd.h>
#endif

namespace scan {

#if defined(_WIN32)
namespace {
fs::path environmentPath (const wchar_t* name)
{
    std::wstring buf (32768, L'\0');
    const auto n = GetEnvironmentVariableW (name, buf.data(), static_cast<DWORD> (buf.size()));
    if (n == 0 || n >= buf.size())
        return {};
    buf.resize (n);
    return fs::path (buf);
}
} // namespace
#endif

std::string osName()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string osVersion()
{
#if defined(_WIN32)
    // GetVersionEx lies to unmanifested programs; RtlGetVersion does not.
    using RtlGetVersionFn = LONG (WINAPI*) (PRTL_OSVERSIONINFOW);
    if (auto ntdll = GetModuleHandleW (L"ntdll.dll"))
    {
        if (auto fn = reinterpret_cast<RtlGetVersionFn> (reinterpret_cast<void*> (GetProcAddress (ntdll, "RtlGetVersion"))))
        {
            RTL_OSVERSIONINFOW info {};
            info.dwOSVersionInfoSize = sizeof (info);
            if (fn (&info) == 0)
                return std::to_string (info.dwMajorVersion) + "." + std::to_string (info.dwMinorVersion)
                     + "." + std::to_string (info.dwBuildNumber);
        }
    }
    return "unknown";
#elif defined(__APPLE__)
    char buf[64] {};
    size_t size = sizeof (buf) - 1;
    if (sysctlbyname ("kern.osproductversion", buf, &size, nullptr, 0) == 0)
        return buf;
    return "unknown";
#else
    utsname u {};
    if (uname (&u) == 0)
        return u.release;
    return "unknown";
#endif
}

std::string archName()
{
#if defined(_M_ARM64EC)
    return "arm64ec";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::vector<std::string> bundleArchitectureFolders()
{
    // From the VST3 bundle format: Contents/<architecture>-<platform>/.
#if defined(_WIN32)
  #if defined(_M_ARM64EC)
    return { "arm64ec-win", "arm64x-win", "x86_64-win" };
  #elif defined(_M_ARM64)
    return { "arm64-win", "arm64x-win" };
  #elif defined(_M_X64)
    return { "x86_64-win", "arm64x-win" };
  #else
    return { "x86-win" };
  #endif
#elif defined(__APPLE__)
    return {};
#else
  #if defined(__aarch64__)
    return { "aarch64-linux" };
  #elif defined(__x86_64__)
    return { "x86_64-linux" };
  #else
    return { "i386-linux" };
  #endif
#endif
}

fs::path homeFolder()
{
#if defined(_WIN32)
    return environmentPath (L"USERPROFILE");
#else
    if (const char* p = std::getenv ("HOME"); p && *p)
        return fs::path (p);
    if (const passwd* pw = getpwuid (getuid()); pw && pw->pw_dir)
        return fs::path (pw->pw_dir);
    return {};
#endif
}

std::vector<fs::path> defaultVst3Folders()
{
    std::vector<fs::path> candidates;
#if defined(_WIN32)
    if (auto p = environmentPath (L"CommonProgramFiles"); ! p.empty())
        candidates.push_back (p / "VST3");
    if (auto p = environmentPath (L"LOCALAPPDATA"); ! p.empty())
        candidates.push_back (p / "Programs" / "Common" / "VST3");
#elif defined(__APPLE__)
    candidates.push_back (homeFolder() / "Library" / "Audio" / "Plug-Ins" / "VST3");
    candidates.push_back ("/Library/Audio/Plug-Ins/VST3");
#else
    candidates.push_back (homeFolder() / ".vst3");
    candidates.push_back ("/usr/lib/vst3");
    candidates.push_back ("/usr/local/lib/vst3");
#endif

    std::vector<fs::path> found;
    for (const auto& c : candidates)
    {
        std::error_code ec;
        if (! c.empty() && fs::is_directory (c, ec))
            found.push_back (c);
    }
    return found;
}

fs::path currentExecutable()
{
#if defined(_WIN32)
    std::wstring buf (32768, L'\0');
    const auto n = GetModuleFileNameW (nullptr, buf.data(), static_cast<DWORD> (buf.size()));
    buf.resize (n);
    return fs::path (buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath (nullptr, &size);
    std::string buf (size, '\0');
    if (_NSGetExecutablePath (buf.data(), &size) != 0)
        return {};
    buf.resize (std::char_traits<char>::length (buf.c_str()));
    std::error_code ec;
    auto p = fs::canonical (buf, ec);
    return ec ? fs::path (buf) : p;
#else
    std::error_code ec;
    return fs::read_symlink ("/proc/self/exe", ec);
#endif
}

std::vector<std::string> commandLineUtf8 (int argc, char** argv)
{
    std::vector<std::string> out;
#if defined(_WIN32)
    (void) argc; (void) argv;
    int n = 0;
    if (LPWSTR* wide = CommandLineToArgvW (GetCommandLineW(), &n))
    {
        for (int i = 0; i < n; ++i)
        {
            const auto u8 = fs::path (wide[i]).u8string();
            out.emplace_back (u8.begin(), u8.end());
        }
        LocalFree (wide);
    }
#else
    for (int i = 0; i < argc; ++i)
        out.emplace_back (argv[i]);
#endif
    return out;
}

fs::path pathFromUtf8 (const std::string& s)
{
    return fs::path (std::u8string (s.begin(), s.end()));
}

std::string displayPath (const fs::path& p)
{
    const auto u8 = p.generic_u8string();
    return std::string (u8.begin(), u8.end());
}

std::string withoutHome (const std::string& text)
{
    const auto home = homeFolder();
    if (home.empty())
        return text;

    std::string result = text;
    // Both spellings, since logs on Windows may use either separator.
    for (auto form : { displayPath (home), [&] { auto u8 = home.u8string(); return std::string (u8.begin(), u8.end()); }() })
    {
        if (form.size() < 2)
            continue;
        for (std::size_t pos = result.find (form); pos != std::string::npos; pos = result.find (form, pos + 1))
            result.replace (pos, form.size(), "~");
    }
    return result;
}

} // namespace scan
