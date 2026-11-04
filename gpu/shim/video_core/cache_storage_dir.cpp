// JStarsReborn: directory-backed shader/pipeline cache, in place of upstream's miniz archive (video_core/cache_storage.cpp).
// Enabled when the runtime names a cache directory (--shader-cache, environment JSTARS_SHADER_CACHE; see
// EmulatorSettings::IsPipelineCacheEnabled and Common::FS::GetUserPath(CacheDir)). The pipeline cache records every shader
// (SPIR-V and metadata) and pipeline key it compiles; at the next start PipelineCache::WarmUp rebuilds them all before the game
// runs, so pipelines are no longer compiled in the middle of a battle (each first use cost a 30-160 ms hitch).
// Blobs are written by a background thread as <name>.<ext>, first to a temporary file and then renamed, so a run that is killed
// while writing never leaves a truncated blob. Existing blobs are never rewritten (names are content hashes). GPL-2.0-or-later.
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <system_error>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "video_core/cache_storage.h"

namespace Storage {
namespace {
std::mutex g_mutex;
std::condition_variable_any g_cv;
std::deque<std::pair<std::filesystem::path, std::vector<u8>>> g_queue;
unsigned g_written, g_failed;

const char* Extension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: return ".meta";
    case BlobType::ShaderBinary: return ".spv";
    case BlobType::PipelineKey: return ".key";
    case BlobType::ShaderProfile: return ".bin";
    }
    return ".blob";
}

void WriteBlob(const std::filesystem::path& path, const std::vector<u8>& data) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) { ++g_failed; f.close(); std::filesystem::remove(tmp, ec); return; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { ++g_failed; std::filesystem::remove(tmp, ec); } else ++g_written;
}

std::vector<u8> ReadBlob(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto size = static_cast<std::streamsize>(f.tellg());
    if (size <= 0) return {};
    std::vector<u8> data(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f) return {};
    return data;
}
} // namespace

void DataBase::Open() {
    if (opened || !EmulatorSettings.IsPipelineCacheEnabled()) return;
    cache_path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir);
    std::error_code ec;
    std::filesystem::create_directories(cache_path, ec);
    if (ec) {
        LOG_WARNING(Render, "shader cache: cannot create {}: {}; running without it", cache_path.string(), ec.message());
        return;
    }
    // Caches of other runtime builds are of no use (and may hold shaders from an older recompiler): remove them.
    for (const auto& e : std::filesystem::directory_iterator(cache_path.parent_path(), ec)) {
        const auto name = e.path().filename().string();
        if (e.is_directory() && name.starts_with("jsc-") && e.path() != cache_path) std::filesystem::remove_all(e.path(), ec);
    }
    io_worker = std::jthread([](std::stop_token stop) {
        for (;;) {
            std::pair<std::filesystem::path, std::vector<u8>> job;
            {
                std::unique_lock lk{g_mutex};
                g_cv.wait(lk, stop, [] { return !g_queue.empty(); });
                if (g_queue.empty()) return; // stop requested and nothing left to write
                job = std::move(g_queue.front());
                g_queue.pop_front();
            }
            WriteBlob(job.first, job.second);
        }
    });
    opened = true;
    LOG_INFO(Render, "shader cache: {}", cache_path.string());
}

void DataBase::Close() {
    if (!opened) return;
    io_worker.request_stop(); // the worker writes what is queued, then ends
    g_cv.notify_all();
    io_worker.join();
    opened = false;
    LOG_INFO(Render, "shader cache: closed ({} blobs written, {} failed)", g_written, g_failed);
}

void DataBase::FinishPreload() {}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) return false;
    {
        std::scoped_lock lk{g_mutex};
        g_queue.emplace_back(cache_path / (name + Extension(type)), std::move(data));
    }
    g_cv.notify_one();
    return true;
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    std::vector<u8> bytes(data.size() * sizeof(u32));
    std::memcpy(bytes.data(), data.data(), bytes.size());
    return Save(type, name, std::move(bytes));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    data.clear();
    if (opened) data = ReadBlob(cache_path / (name + Extension(type)));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    data.clear();
    if (!opened) return;
    const auto bytes = ReadBlob(cache_path / (name + Extension(type)));
    data.resize(bytes.size() / sizeof(u32));
    std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    if (!opened) return;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(cache_path, ec)) {
        if (!e.is_regular_file() || e.path().extension() != Extension(type)) continue;
        auto data = ReadBlob(e.path());
        if (!data.empty()) func(std::move(data));
    }
}
} // namespace Storage
