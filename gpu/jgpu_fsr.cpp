// JStarsReborn - AMD FSR SDK capability probe (graphics milestone B, docs/GRAPHICS.md).
//
// Loads AMD's signed Vulkan loader DLL (amd_fidelityfx_loader_vk.dll) if it is present next to the runtime, and asks it,
// for this machine's Vulkan device, which upscaler and frame-generation versions it can provide. Nothing is created or
// dispatched: this only reports what is available, so the integration can pick FSR 4 upscaling and FSR 3 frame generation
// by what the device really supports. The DLLs are AMD binaries and are not part of this project's source.
//
// This file uses the SDK's C API only (vendor/externals/ffx-api, MIT-licensed headers).
#include "jgpu.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PLATFORM_WINDOWS 1
#include "Kits/FidelityFX/api/include/ffx_api.h"
#include "Kits/FidelityFX/api/include/ffx_api_loader.h"
#include "Kits/FidelityFX/api/include/vk/ffx_api_vk.h"
#include "Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#endif

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace {
std::string g_report = "FidelityFX probe not run";
}

#ifdef _WIN32
static std::string list_versions(PfnFfxQuery query, uint64_t create_type, VkDeviceContext* dc, const char* what) {
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = create_type;
    q.device = dc;
    q.outputCount = &count;
    ffxReturnCode_t rc = query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) return std::string(what) + ": query failed (code " + std::to_string(rc) + ")";
    if (count == 0) return std::string(what) + ": no provider for this device";
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count, nullptr);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    rc = query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) return std::string(what) + ": version list failed (code " + std::to_string(rc) + ")";
    std::string s = std::string(what) + ":";
    for (uint64_t i = 0; i < count; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof buf, " [%s id 0x%llx]", names[i] ? names[i] : "?", static_cast<unsigned long long>(ids[i]));
        s += buf;
    }
    return s + " (first = default)";
}
#endif

int jgpu_fsr_probe(void) {
#ifndef _WIN32
    g_report = "FidelityFX probe: Windows only";
    return 1;
#else
    if (!presenter) { g_report = "FidelityFX probe: no presenter yet"; return 1; }
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    HMODULE mod = LoadLibraryW((dir + L"amd_fidelityfx_loader_vk.dll").c_str());
    if (!mod) {
        g_report = "FidelityFX probe: amd_fidelityfx_loader_vk.dll is not next to the runtime; FSR upscaling and frame generation are unavailable";
        LOG_INFO(Lib, "{}", g_report);
        return 1;
    }
    ffxFunctions f{};
    ffxLoadFunctions(&f, mod);
    if (!f.Query || !f.CreateContext) { g_report = "FidelityFX probe: the loader DLL lacks the expected exports"; LOG_ERROR(Lib, "{}", g_report); return 1; }
    const auto& inst = presenter->GetInstance();
    VkDeviceContext dc{};
    dc.vkDevice = static_cast<VkDevice>(inst.GetDevice());
    dc.vkPhysicalDevice = static_cast<VkPhysicalDevice>(inst.GetPhysicalDevice());
    dc.vkDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkGetDeviceProcAddr"));
    if (!dc.vkDeviceProcAddr) { g_report = "FidelityFX probe: vkGetDeviceProcAddr not found"; LOG_ERROR(Lib, "{}", g_report); return 1; }
    const std::string up = list_versions(f.Query, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &dc, "upscaler");
    const std::string fg = list_versions(f.Query, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &dc, "frame generation");
    // What render size each quality mode implies for a 3840x2160 output (null-context query with the device chained).
    std::string ratios;
    for (uint32_t mode : {FFX_UPSCALE_QUALITY_MODE_NATIVEAA, FFX_UPSCALE_QUALITY_MODE_QUALITY, FFX_UPSCALE_QUALITY_MODE_BALANCED,
                          FFX_UPSCALE_QUALITY_MODE_PERFORMANCE}) {
        float ratio = 0;
        ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode rq{};
        rq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE;
        rq.qualityMode = mode;
        rq.pOutUpscaleRatio = &ratio;
        ffxCreateBackendVKDesc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.deviceContext = dc;
        rq.header.pNext = &be.header;
        if (f.Query(nullptr, &rq.header) == FFX_API_RETURN_OK) {
            char b[48];
            std::snprintf(b, sizeof b, " mode%u=%.2fx", mode, ratio);
            ratios += b;
        }
    }
    g_report = "FidelityFX probe (SDK 2.4.0 loader): " + up + "; " + fg + "; upscale ratios" + (ratios.empty() ? std::string(" unavailable") : ratios);
    LOG_INFO(Lib, "{}", g_report);
    return 0;
#endif
}

const char* jgpu_fsr_report(void) { return g_report.c_str(); }
