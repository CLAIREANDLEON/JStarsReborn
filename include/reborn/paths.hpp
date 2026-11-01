#pragma once
#include <filesystem>
#include <string>

namespace reborn {
// Per-user writable data root: %LOCALAPPDATA%\JStarsReborn on Windows (resolved through
// the Known Folder API, not the environment variable). Settings, logs, crash reports and
// setup reports live under it, so the program folder can be read-only (e.g. Program Files).
// Throws std::runtime_error if the folder cannot be resolved.
[[nodiscard]] std::filesystem::path default_data_dir();

enum class MigrationResult {
    NoLegacySettings, // nothing to migrate
    AlreadyPresent,   // per-user settings exist; legacy file ignored and untouched
    Migrated,         // legacy settings copied (original preserved)
};
// One-time, copy-only settings migration from the pre-0.2.2 location (config\settings.ini
// beside the executable) into the per-user data root. The legacy file is never modified
// or deleted. The copy refuses to overwrite an existing destination, so a concurrent or
// repeated launch cannot clobber settings already in the per-user location. Content is
// copied verbatim; validation happens at load time, which preserves a rejected file.
// Throws std::filesystem::filesystem_error on I/O failure.
MigrationResult migrate_legacy_settings(const std::filesystem::path& legacy_settings,
                                        const std::filesystem::path& per_user_settings);
[[nodiscard]] const char* to_string(MigrationResult result) noexcept;
}
