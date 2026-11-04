// JStarsReborn: path_util replacement. Every PathType is a subdirectory of the per-run data directory
// handed to jgpu_init; the game directory is never written (see docs/ADR-005 and the input rules).
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <system_error>
#include "common/path_util.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
extern "C" const char* jgpu_data_dir(void);

namespace Common::FS {
bool ValidatePath(const std::filesystem::path& path) { return !path.empty(); }
std::string PathToUTF8String(const std::filesystem::path& path) { return path.generic_string(); }
static std::filesystem::path g_paths[32];
static bool g_init;
static std::mutex g_mutex;
static const char* DirName(PathType t) {
    switch (t) {
    case PathType::LogDir: return LOG_DIR;
    case PathType::ScreenshotsDir: return SCREENSHOTS_DIR;
    case PathType::ShaderDir: return SHADER_DIR;
    case PathType::TempDataDir: return TEMPDATA_DIR;
    case PathType::CacheDir: return CACHE_DIR;
    case PathType::CapturesDir: return CAPTURES_DIR;
    default: return "misc";
    }
}
// The shader cache (--shader-cache, environment JSTARS_SHADER_CACHE) lives outside the per-run directory, in a subdirectory
// named after the running executable's size and modification time: another runtime build (possibly with a different shader
// recompiler) never reuses it.
static std::filesystem::path ShaderCacheDir() {
    const char* base = std::getenv("JSTARS_SHADER_CACHE");
    if (!base || !*base) return {};
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (n && n < 32768) exe = std::filesystem::path(std::wstring(buf, n));
#else
    std::error_code ec0;
    exe = std::filesystem::read_symlink("/proc/self/exe", ec0);
#endif
    std::error_code ec;
    const auto size = exe.empty() ? 0 : std::filesystem::file_size(exe, ec);
    const auto time = exe.empty() ? 0 : std::filesystem::last_write_time(exe, ec).time_since_epoch().count();
    char name[64];
    std::snprintf(name, sizeof name, "jsc-%llx-%llx", static_cast<unsigned long long>(size), static_cast<unsigned long long>(time));
    // The value is UTF-8 (the runtime takes its arguments from the wide command line as UTF-8).
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base))) / name;
}

const std::filesystem::path& GetUserPath(PathType user_path) {
    std::scoped_lock lk{g_mutex};
    auto& slot = g_paths[static_cast<int>(user_path)];
    if (slot.empty() && user_path == PathType::CacheDir) slot = ShaderCacheDir();
    if (slot.empty()) {
        const char* root = jgpu_data_dir();
        slot = std::filesystem::path(root ? root : ".") / (user_path == PathType::UserDir ? "" : DirName(user_path));
    }
    return slot;
}
std::string GetUserPathString(PathType user_path) { return GetUserPath(user_path).generic_string(); }
void SetUserPath(PathType user_path, const std::filesystem::path& new_path) {
    std::scoped_lock lk{g_mutex};
    g_paths[static_cast<int>(user_path)] = new_path;
}
std::optional<std::filesystem::path> FindGameByID(const std::filesystem::path&, const std::string&, int) { return std::nullopt; }
} // namespace Common::FS
