#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace reborn::setup {
struct InventoryResult {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    bool complete = true;
    // Inventory never establishes game compatibility. A verified importer is absent.
    bool game_import_supported = false;
    std::vector<std::string> diagnostics;
};

// Inventories static, legitimately supplied local files without modifying them.
// The destination must be new and outside source; missing report parents are
// created only after successful scanning and overlap checks.
// Cancellation throws and leaves no destination or temporary report behind.
InventoryResult inventory_folder(
    const std::filesystem::path& source,
    const std::filesystem::path& report,
    std::stop_token stop = {},
    std::function<void(std::uint64_t, std::uint64_t)> progress = {});
}
