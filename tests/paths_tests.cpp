// Portable tests for the per-user data root and the copy-only legacy settings migration.
#include "reborn/paths.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
int checks = 0, skips = 0;
void check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary); o << s;
    if (!o) throw std::runtime_error("fixture write failed");
}
std::string get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(i), std::istreambuf_iterator<char>()};
}
}

int main() {
    fs::path root;
    try {
        using reborn::MigrationResult;
        const auto data = reborn::default_data_dir();
        check(data.filename() == "JStarsReborn", "Per-user data root is named JStarsReborn");
        check(data.is_absolute(), "Per-user data root is absolute");

        root = fs::temp_directory_path() / ("reborn-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(fs::create_directory(root), "fresh fixture root");
        const auto legacy = root / "install" / "config" / "settings.ini";
        const auto per_user = root / "localappdata" / "JStarsReborn" / "config" / "settings.ini";
        const std::string original = "# legacy\nwidth=2560\nheight=1440\nvsync=0\nfixed_hz=60\nmaster_volume=0.25\n";

        check(reborn::migrate_legacy_settings(legacy, per_user) == MigrationResult::NoLegacySettings, "No legacy file: nothing done");
        check(!fs::exists(per_user), "No legacy file: nothing created");

        put(legacy, original);
        check(reborn::migrate_legacy_settings(legacy, per_user) == MigrationResult::Migrated, "Legacy settings migrated");
        check(get(per_user) == original, "Migrated bytes are verbatim");
        check(get(legacy) == original, "Legacy file untouched");

        put(per_user, "width=1920\n");
        put(legacy, original + "# edited later\n");
        check(reborn::migrate_legacy_settings(legacy, per_user) == MigrationResult::AlreadyPresent, "Existing per-user settings win");
        check(get(per_user) == "width=1920\n", "Existing per-user settings never overwritten");
        check(get(legacy) == original + "# edited later\n", "Legacy file still untouched");

        const auto dir_legacy = root / "dir-install" / "config" / "settings.ini";
        fs::create_directories(dir_legacy);
        const auto dir_target = root / "dir-target" / "settings.ini";
        check(reborn::migrate_legacy_settings(dir_legacy, dir_target) == MigrationResult::NoLegacySettings, "Directory is not migrated");
        check(!fs::exists(dir_target), "Directory produced no target");

        const auto link_legacy = root / "link-install" / "settings.ini";
        const auto link_target = root / "link-target" / "settings.ini";
        fs::create_directories(link_legacy.parent_path());
        std::error_code ec;
        fs::create_symlink(legacy, link_legacy, ec);
        if (ec || !fs::is_symlink(fs::symlink_status(link_legacy, ec))) {
            ++skips;
            std::cout << "SKIP symbolic-link fixture unavailable on this host\n";
        } else {
            check(reborn::migrate_legacy_settings(link_legacy, link_target) == MigrationResult::NoLegacySettings, "Symlinked legacy file is not followed");
            check(!fs::exists(link_target), "Symlink produced no target");
        }
        fs::remove_all(root);
        std::cout << "PASS: " << checks << " data-path checks, " << skips << " environment skips\n";
        return 0;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); }
        return 1;
    }
}
