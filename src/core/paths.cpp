#include "reborn/paths.hpp"

#include <cstdlib>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace reborn {
#ifdef _WIN32
std::filesystem::path default_data_dir() {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (raw) CoTaskMemFree(raw);
        throw std::runtime_error("Cannot resolve the per-user Local AppData folder");
    }
    std::filesystem::path base(raw);
    CoTaskMemFree(raw);
    return base / L"JStarsReborn";
}
#else
// Non-Windows builds exist only for portable tests; follow the XDG convention there.
std::filesystem::path default_data_dir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "JStarsReborn";
    if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home) / ".local" / "share" / "JStarsReborn";
    throw std::runtime_error("Cannot resolve a per-user data folder");
}
#endif

MigrationResult migrate_legacy_settings(const std::filesystem::path& legacy_settings,
                                        const std::filesystem::path& per_user_settings) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(per_user_settings, ec))) return MigrationResult::AlreadyPresent;
    ec.clear();
    const auto legacy_status = fs::symlink_status(legacy_settings, ec);
    // Only a regular file is migrated; links and other objects are never followed.
    if (ec || !fs::is_regular_file(legacy_status)) return MigrationResult::NoLegacySettings;
    if (!per_user_settings.parent_path().empty()) fs::create_directories(per_user_settings.parent_path());
    // copy_options::none fails if the destination appeared meanwhile: never overwrite.
    if (!fs::copy_file(legacy_settings, per_user_settings, fs::copy_options::none)) return MigrationResult::AlreadyPresent;
    return MigrationResult::Migrated;
}

const char* to_string(MigrationResult result) noexcept {
    switch (result) {
    case MigrationResult::NoLegacySettings: return "no legacy settings";
    case MigrationResult::AlreadyPresent: return "per-user settings already present; legacy file left untouched";
    case MigrationResult::Migrated: return "legacy settings copied; original left in place";
    }
    return "unknown";
}
}
