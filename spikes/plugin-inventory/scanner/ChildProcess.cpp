// Spike code (issue #14). See ChildProcess.h.

#include "ChildProcess.h"

#include <cstdio>
#include <thread>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <csignal>
  #include <cstring>
  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
  extern char** environ;
#endif

namespace scan {

namespace {

double millisecondsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

} // namespace

#if defined(_WIN32)

namespace {

// Quotes one argument the way the Microsoft C runtime parses it back.
std::wstring quoteArgument (const std::wstring& arg)
{
    if (! arg.empty() && arg.find_first_of (L" \t\n\v\"") == std::wstring::npos)
        return arg;

    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it)
    {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }

        if (it == arg.end())
        {
            out.append (backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
        {
            out.append (backslashes * 2 + 1, L'\\');
            out.push_back (L'"');
        }
        else
        {
            out.append (backslashes, L'\\');
            out.push_back (*it);
        }
    }
    out.push_back (L'"');
    return out;
}

} // namespace

ChildResult runChild (const std::filesystem::path& program,
                      const std::vector<std::filesystem::path>& args,
                      const std::filesystem::path& log,
                      std::chrono::milliseconds timeout)
{
    ChildResult r;
    const auto start = std::chrono::steady_clock::now();

    std::wstring commandLine = quoteArgument (program.wstring());
    for (const auto& a : args)
        commandLine += L" " + quoteArgument (a.wstring());

    SECURITY_ATTRIBUTES sa { sizeof (sa), nullptr, TRUE };
    HANDLE logHandle = CreateFileW (log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE)
        return r;

    // A job object, so killing the probe also kills anything a plugin started.
    HANDLE job = CreateJobObjectW (nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
    }

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = logHandle;
    si.hStdError = logHandle;

    PROCESS_INFORMATION pi {};
    const BOOL started = CreateProcessW (nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle (logHandle);
    if (! started)
    {
        if (job) CloseHandle (job);
        return r;
    }

    if (job)
        AssignProcessToJobObject (job, pi.hProcess);
    ResumeThread (pi.hThread);
    CloseHandle (pi.hThread);

    const auto waited = WaitForSingleObject (pi.hProcess, static_cast<DWORD> (timeout.count()));
    if (waited == WAIT_TIMEOUT)
    {
        if (job) TerminateJobObject (job, 1);
        else TerminateProcess (pi.hProcess, 1);
        WaitForSingleObject (pi.hProcess, 5000);
        r.ending = ChildResult::Ending::timedOut;
    }
    else
    {
        DWORD code = 0;
        GetExitCodeProcess (pi.hProcess, &code);
        // NTSTATUS error codes (0xC0000000 and up) mean an unhandled exception,
        // such as 0xC0000005 for an access violation. abort() exits with 3.
        if (code >= 0xC0000000u)
        {
            r.ending = ChildResult::Ending::crashed;
            char buf[32];
            std::snprintf (buf, sizeof (buf), "exception 0x%08lX", static_cast<unsigned long> (code));
            r.crashDetail = buf;
        }
        else if (code == 3)
        {
            r.ending = ChildResult::Ending::crashed;
            r.crashDetail = "abort (exit code 3)";
        }
        else
        {
            r.ending = ChildResult::Ending::exited;
            r.exitCode = static_cast<int> (code);
        }
    }

    CloseHandle (pi.hProcess);
    if (job) CloseHandle (job);
    r.elapsedMs = millisecondsSince (start);
    return r;
}

#else // macOS and Linux

ChildResult runChild (const std::filesystem::path& program,
                      const std::vector<std::filesystem::path>& args,
                      const std::filesystem::path& log,
                      std::chrono::milliseconds timeout)
{
    ChildResult r;
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::string> argStrings;
    argStrings.push_back (program.string());
    for (const auto& a : args)
        argStrings.push_back (a.string());
    std::vector<char*> argv;
    for (auto& s : argStrings)
        argv.push_back (s.data());
    argv.push_back (nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_addopen (&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2 (&actions, STDOUT_FILENO, STDERR_FILENO);

    // Its own process group, so a kill reaches anything a plugin started.
    posix_spawnattr_t attr;
    posix_spawnattr_init (&attr);
    posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup (&attr, 0);

    pid_t pid = 0;
    const int rc = posix_spawn (&pid, argStrings[0].c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy (&actions);
    posix_spawnattr_destroy (&attr);
    if (rc != 0)
        return r;

    const auto deadline = start + timeout;
    int status = 0;
    for (;;)
    {
        const pid_t done = waitpid (pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR)
        {
            r.ending = ChildResult::Ending::couldNotStart;
            return r;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            kill (-pid, SIGKILL);
            waitpid (pid, &status, 0);
            r.ending = ChildResult::Ending::timedOut;
            r.elapsedMs = millisecondsSince (start);
            return r;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    // The probe has exited; clear up anything it left running in its group.
    kill (-pid, SIGKILL);

    if (WIFSIGNALED (status))
    {
        r.ending = ChildResult::Ending::crashed;
        const int sig = WTERMSIG (status);
        const char* name = strsignal (sig);
        r.crashDetail = "signal " + std::to_string (sig) + (name ? std::string (" (") + name + ")" : std::string());
    }
    else
    {
        r.ending = ChildResult::Ending::exited;
        r.exitCode = WEXITSTATUS (status);
    }
    r.elapsedMs = millisecondsSince (start);
    return r;
}

#endif

} // namespace scan
