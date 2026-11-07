// JStarsReborn: the settings the vendored shadPS4 video core reads. Values come from JSTARS_GPU_*
// environment variables (read once); defaults are conservative and match upstream where it matters.
// Interface follows shadPS4 b4e7ae77 core/emulator_settings.h (names used by the vendored code only).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include "common/types.h"

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { static const auto value = s32(Number("JSTARS_GPU_ID", -1)); return value; }
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("JSTARS_INTERNAL_WIDTH", 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("JSTARS_INTERNAL_HEIGHT", 1080)); return value; }
    // Mailbox by default, as upstream shadPS4: presenting never blocks the vblank loop (FIFO made it wait for the monitor's own refresh,
    // e.g. 175 Hz, and the loop then caught up with back-to-back vblanks: stutter). Falls back to FIFO where Mailbox is unsupported.
    std::string GetPresentMode() { const char* v = std::getenv("JSTARS_PRESENT_MODE"); return v ? v : "Mailbox"; }
    u32 GetReadbacksMode() { static const auto value = u32(Number("JSTARS_READBACKS", GpuReadbacksMode::Disabled)); return value; }
    u32 GetVblankFrequency() { static const auto value = u32(std::max(1L, Number("JSTARS_VBLANK_HZ", 60))); return value; }
    // Low latency (videoout/driver.cpp, PresentThread). 0: every frame is shown at its flip, as in shadPS4. 1: early present, a
    // finished frame is shown as soon as it is due instead of waiting for its flip. 2 (default): also late frame start, the game
    // is woken for its next frame only as late as it can still make the next flip, so its controller read is fresher. The flips
    // the game sees keep their vblank timing at every level. Runtime --low-latency <0|1|2>.
    int GetLowLatencyLevel() { static const auto value = int(std::clamp(Number("JSTARS_LOW_LATENCY", 2), 0L, 2L)); return value; }
    // Refresh lock (videoout/driver.cpp, PresentThread): the vblank timer runs at the display's exact refresh period (times a
    // divisor) and is phased so that frames are presented just before a display vblank. Runtime --no-refresh-lock.
    bool IsRefreshLockEnabled() { static const auto value = Flag("JSTARS_REFRESH_LOCK", true); return value; }
    // "present timing" log line every five seconds (frame ready, shown, input read to picture): runtime --frame-stats.
    bool IsFrameStatsEnabled() { static const auto value = Flag("JSTARS_FRAME_STATS", false); return value; }
    bool IsCopyGpuBuffers() { static const auto value = Flag("JSTARS_COPY_GPU_BUFFERS", false); return value; }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("JSTARS_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("JSTARS_DUMP_SHADERS", false); return value; }
    bool IsHdrAllowed() { return false; }
    bool IsNullGPU() { static const auto value = Flag("JSTARS_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    // On-disk shader/pipeline cache (shim/video_core/cache_storage_dir.cpp): on when the runtime names a directory (--shader-cache).
    bool IsPipelineCacheEnabled() { static const bool value = [] { const char* v = std::getenv("JSTARS_SHADER_CACHE"); return v && *v; }(); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("JSTARS_READBACK_LINEAR", false); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsUserfaultfdTracking() { return false; } // page tracking uses the fault handler on every host
    bool IsVkCrashDiagnosticEnabled() { static const auto value = Flag("JSTARS_VK_CRASH_DIAG", false); return value; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("JSTARS_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("JSTARS_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("JSTARS_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { static const auto value = Flag("JSTARS_VK_VALIDATION_GPU", false); return value; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("JSTARS_VK_VALIDATION_SYNC", false); return value; }
    // AMD FSR 1 (EASU upscaling + RCAS sharpening, upstream host_passes/fsr_pass.cpp) when the game's frame is smaller than the
    // window: runtime --fsr. Sharpness: RCAS attenuation in stops (0 = sharpest, 2 = softest), runtime --fsr-sharpness.
    bool IsFsrEnabled() { static const auto value = Flag("JSTARS_FSR", false); return value; }
    bool IsRcasEnabled() { static const auto value = Flag("JSTARS_FSR", false); return value; }
    float GetRcasAttenuation() { static const auto value = static_cast<float>(Number("JSTARS_FSR_ATTENUATION_MILLI", 250)); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
