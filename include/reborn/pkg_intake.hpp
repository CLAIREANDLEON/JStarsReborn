#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
namespace reborn::setup {
struct PkgResult {
    std::uint64_t bytes=0;
    std::string sha256;
    std::string content_id;
    bool game_import_supported=false;
};
// Registers outer-header metadata and a hash. Does not extract, decrypt or convert content.
PkgResult inspect_pkg(const std::filesystem::path& pkg,const std::filesystem::path& report,
    std::stop_token stop={},std::function<void(std::uint64_t,std::uint64_t)> progress={});
}
