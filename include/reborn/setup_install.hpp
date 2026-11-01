#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
namespace reborn::setup {
struct InstallResult {
    std::filesystem::path executable;
    uint64_t files = 0;
    uint64_t bytes = 0;
};
// Installs only the hash-verified local manifest contents into a NEW directory.
// Hashes establish payload integrity, not publisher authenticity. No game import.
InstallResult install_foundation(const std::filesystem::path& package_root,
    const std::filesystem::path& destination, std::stop_token stop = {},
    std::function<void(uint64_t, uint64_t)> progress = {});
}
