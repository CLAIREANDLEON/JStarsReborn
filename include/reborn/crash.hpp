#pragma once
#include <filesystem>
#include <string_view>

namespace reborn {
// Installs a process-wide unhandled-exception filter and std::terminate handler.
// On a crash it writes crashes/crash-<utc>-<pid>.json (exception, faulting module+RVA,
// unwound stack as module+RVA frames) and a matching .dmp minidump when dbghelp is
// available, then terminates the process with the original exception code. It never
// resumes execution or hides the failure. Not reentrant-safe for nested crashes beyond
// a single guard: a second fault while reporting terminates immediately.
void install_crash_reporter(const std::filesystem::path& crash_directory);

// Deliberate failures for verifying the reporter: "access" (access violation) or
// "terminate" (std::terminate via an unhandled C++ exception path).
[[noreturn]] void trigger_test_crash(std::wstring_view kind);
}
