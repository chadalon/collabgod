// Running one probe in a child process with a time limit. Spike code (issue #14).

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace scan {

struct ChildResult
{
    enum class Ending { exited, crashed, timedOut, couldNotStart };

    Ending ending = Ending::couldNotStart;
    int exitCode = 0;          // when exited
    std::string crashDetail;   // when crashed: a signal name or an exception code
    double elapsedMs = 0;
};

// Starts `program args...` with stdout and stderr written to `log`, and waits up
// to `timeout`. A child still running at the deadline is killed, along with any
// processes it started (its process group on macOS and Linux, its job on Windows).
ChildResult runChild (const std::filesystem::path& program,
                      const std::vector<std::filesystem::path>& args,
                      const std::filesystem::path& log,
                      std::chrono::milliseconds timeout);

} // namespace scan
