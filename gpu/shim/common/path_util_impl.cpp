// JStarsReborn: path_util replacement. Every PathType is a subdirectory of the per-run data directory
// handed to jgpu_init; the game directory is never written (see docs/ADR-005 and the input rules).
#include <mutex>
#include "common/path_util.h"
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
const std::filesystem::path& GetUserPath(PathType user_path) {
    std::scoped_lock lk{g_mutex};
    auto& slot = g_paths[static_cast<int>(user_path)];
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
