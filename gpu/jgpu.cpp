// JStarsReborn GPU library glue: hosts the vendored shadPS4 video core inside the native runtime.
// GPL-2.0-or-later. Replaces what shadPS4's emulator.cpp/memory manager/signal layer provide to the
// video core; everything else is the vendored code (see gpu/VENDOR.md for the boundary).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jgpu.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/debug_state.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/equeue.h"
namespace Libraries::Kernel { void StartKernelService(); void StopKernelService(); }
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

// ---------------------------------------------------------------- host services
static JgpuHost g_host{};
static std::string g_data_dir = ".", g_capture_dir = ".";

JgpuHost* jgpu_host_ptr();
extern "C" {
void (*jgpu_log_sink)(const char* line) = nullptr;
const char* jgpu_data_dir(void) { return g_data_dir.c_str(); }
const char* jgpu_capture_dir(void) { return g_capture_dir.c_str(); }
void jgpu_fatal(const char* what) {
    if (g_host.fatal) g_host.fatal(what);
    std::fprintf(stderr, "gpu fatal: %s\n", what);
    std::abort();
}
int jgpu_fd_alloc(void) { return g_host.fd_alloc ? g_host.fd_alloc() : -1; }
void jgpu_fd_free(int fd) { if (g_host.fd_free) g_host.fd_free(fd); }
}

JgpuHost* jgpu_host_ptr() { return &g_host; }
void jgpu_set_host(const JgpuHost* host) {
    g_host = *host;
    jgpu_log_sink = g_host.log;
}

// ---------------------------------------------------------------- symbol registry
namespace Core::Loader {
static std::unordered_map<std::string, u64>& Table() {
    static std::unordered_map<std::string, u64> table;
    return table;
}
static std::unordered_map<std::string, std::string>& Names() {
    static std::unordered_map<std::string, std::string> names;
    return names;
}
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char*, SymbolType, u64 address, const char* name) {
    const std::string key = std::string(nid) + "#" + library;
    Table()[key] = address;
    Names()[key] = name;
}
} // namespace Core::Loader

void* jgpu_resolve(const char* nid_lib) {
    auto& t = Core::Loader::Table();
    auto it = t.find(nid_lib);
    return it == t.end() ? nullptr : reinterpret_cast<void*>(it->second);
}
const char* jgpu_symbol_name(const char* nid_lib) {
    auto& n = Core::Loader::Names();
    auto it = n.find(nid_lib);
    return it == n.end() ? "" : it->second.c_str();
}
unsigned jgpu_symbol_count(void) { return static_cast<unsigned>(Core::Loader::Table().size()); }

// ---------------------------------------------------------------- memory manager surface
namespace Core {
static std::mutex g_map_mutex;
static std::vector<std::pair<VAddr, u64>> g_maps; // ranges mapped before the renderer existed
static std::atomic<u64> g_map_count{0};

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    int prot = 0;
    if (True(perms & MemoryPermission::Read)) prot |= 1;
    if (True(perms & MemoryPermission::Write)) prot |= 2;
    if (True(perms & MemoryPermission::Execute)) prot |= 4;
    // Page tracking needs this to work: a silent failure would let the GPU read stale data.
    const int rc = g_host.protect ? g_host.protect(reinterpret_cast<void*>(virtual_addr), size, prot) : -1;
    ASSERT_MSG(rc == 0, "page protection failed for {:#x}+{:#x} (prot {})", virtual_addr, size, prot);
}

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    std::scoped_lock lk{g_map_mutex};
    rasterizer = rasterizer_;
    for (const auto& [addr, size] : g_maps) {
        if (IsValidGpuMapping(addr, size)) rasterizer->MapMemory(addr, size);
    }
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    static constexpr u64 MinSizeToClamp = 1ULL << 30;
    if (size < MinSizeToClamp) return size;
    // Ranges of 1 GiB or more would need the extent of the contiguous mapping; the runtime does not
    // report that yet, so the range is passed through unchanged and the event is logged.
    LOG_WARNING(Kernel_Vmm, "ClampRangeSize({:#x}, {:#x}): mapping extent unknown, size not clamped", virtual_addr, size);
    return size;
}

void MemoryManager::CopySparseMemory(VAddr virtual_addr, u8* dest, u64 size) {
    const size_t got = g_host.safe_read ? g_host.safe_read(dest, reinterpret_cast<const void*>(virtual_addr), size) : 0;
    if (got < size) {
        std::memset(dest + got, 0, size - got); // unmapped guest pages read as zero
    }
}

bool MemoryManager::TryWriteBacking(void*, const void*, u64) {
    // No separate unprotected view of guest memory exists yet: callers write through the guest mapping,
    // where the page tracker handles a write to a protected page (see video_core/page_manager.cpp).
    return false;
}
} // namespace Core

void jgpu_mem_map(uintptr_t addr, uint64_t len) {
    auto* mm = Core::Memory::Instance();
    std::scoped_lock lk{Core::g_map_mutex};
    Core::g_maps.emplace_back(addr, len);
    ++Core::g_map_count;
    if (mm->GetRasterizer() && mm->IsValidGpuMapping(addr, len)) mm->GetRasterizer()->MapMemory(addr, len);
}
void jgpu_mem_unmap(uintptr_t addr, uint64_t len) {
    auto* mm = Core::Memory::Instance();
    std::scoped_lock lk{Core::g_map_mutex};
    std::erase_if(Core::g_maps, [&](const auto& m) { return m.first >= addr && m.first + m.second <= addr + len; });
    if (mm->GetRasterizer() && mm->IsValidGpuMapping(addr, len)) mm->GetRasterizer()->UnmapMemory(addr, len);
}

int jgpu_handle_fault(void* native, uintptr_t address) {
    return Core::Signals::Instance()->DispatchAccessViolation(native, reinterpret_cast<void*>(address)) ? 1 : 0;
}

// ---------------------------------------------------------------- init
Frontend::WindowSDL* g_window = nullptr; // referenced by gnmdriver.cpp
static std::unique_ptr<Frontend::WindowSDL> g_window_owner;
static Core::Loader::SymbolsResolver g_resolver;

int jgpu_init(const JgpuConfig* config) {
    g_data_dir = config->data_dir ? config->data_dir : ".";
    g_capture_dir = config->capture_dir ? config->capture_dir : ".";
    LOG_INFO(Lib, "creating window {}x{}", config->width, config->height);
    g_window_owner = std::make_unique<Frontend::WindowSDL>(config->width, config->height, config->title ? config->title : "JStarsReborn");
    g_window = g_window_owner.get();
    if (!g_window->IsOpen()) {
        LOG_CRITICAL(Lib, "window creation failed");
        return 1;
    }
    Libraries::Kernel::StartKernelService();
    Libraries::Kernel::RegisterEventQueue(&g_resolver);
    Libraries::GnmDriver::RegisterLib(&g_resolver); // creates Liverpool and the Vulkan presenter
    Libraries::VideoOut::RegisterLib(&g_resolver);
    LOG_INFO(Lib, "GPU library ready: {} exports", jgpu_symbol_count());
    return 0;
}

void jgpu_request_capture(int count) {
    for (int i = 0; i < count; ++i) VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
}

void jgpu_get_stats(JgpuStats* out) {
    out->draws = DebugStateInstance.draw_count.load();
    out->dispatches = DebugStateInstance.dispatch_count.load();
    out->flips_requested = DebugStateInstance.flip_count.load();
    out->gnm_frames = DebugStateInstance.gnm_frame_count.load();
    out->mem_maps = Core::g_map_count.load();
}

void jgpu_shutdown(void) {
    Libraries::Kernel::StopKernelService();
    g_window = nullptr;
    g_window_owner.reset();
}
