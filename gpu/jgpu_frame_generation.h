// JStarsReborn - standalone FidelityFX Vulkan frame-generation recorder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#if defined(_WIN32) && !defined(PLATFORM_WINDOWS)
#define PLATFORM_WINDOWS 1
#endif
#include "Kits/FidelityFX/api/include/ffx_api_loader.h"
#include "Kits/FidelityFX/api/include/vk/ffx_api_vk.h"
#include "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"

namespace jgpu::fg {

enum class Stage { ok, invalid_input, unavailable, already_open, enumerate, create,
                   verify_provider, configure, prepare, generate, faulted, destroy };
struct Result {
    Stage stage = Stage::ok;
    ffxReturnCode_t sdk = FFX_API_RETURN_OK;
    // False on reset/first frames, and on every error. Never present an invalid output.
    bool interpolated = false;
    explicit operator bool() const { return stage == Stage::ok; }
};
struct Provider { uint64_t id; std::string name; };

// IDs are opaque, device-specific and must not be persisted across sessions.
Result enumerate(const ffxFunctions&, VkDeviceContext, std::vector<Provider>&);

struct Settings {
    uint64_t provider_id = 0; // Explicit ID from enumerate(); never silently falls back.
    FfxApiDimensions2D display_size{};
    FfxApiDimensions2D max_render_size{};
    uint32_t color_format = FFX_API_SURFACE_FORMAT_UNKNOWN;
    // Async is intentionally unsupported by this serial recorder.
    uint32_t flags = 0;
};

struct Frame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    uint64_t id = 0; // Rendered-frame ID, not a generated-frame/simulation tick counter.
    FfxApiDimensions2D render_size{};
    FfxApiRect2D generation_rect{}; // Explicit content rectangle; no guessed stretch.
    FfxApiResource color{}, depth{}, motion_vectors{}, output{};
    FfxApiFloatCoords2D jitter_pixels{}, motion_vector_scale{};
    float elapsed_ms = 0;
    float camera_near = 0, camera_far = 0, vertical_fov_radians = 0;
    float view_space_to_meters = 0;
    float camera_position[3]{}, camera_up[3]{}, camera_right[3]{}, camera_forward[3]{};
    uint32_t transfer_function = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    float min_luminance = 0, max_luminance = 0;
    bool reset = false;
};

// Single render-thread use. DLL/device must outlive this object. The caller must
// finish or discard recorded command buffers and drain their GPU work BEFORE
// close(), destruction, or reuse of any input/output. See docs/FRAME_GENERATION.md.
// This class records commands only: no queue submission, swapchain or game tick.
class Context final {
public:
    Context() = default;
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    Result open(const ffxFunctions&, VkDeviceContext, const Settings&);
    Result record(const Frame&);
    // Also call when bypassing FG, after abandoning an otherwise successful
    // recording, or on a scene/camera cut. Does not clear an SDK failure.
    void reset_history() { history_ = false; }
    Result close(); // GPU-idle precondition; check failure before unloading DLL.
    const Provider& provider() const { return provider_; }

private:
    bool valid(const Frame&) const;
    Result fail(Stage, ffxReturnCode_t);
    ffxFunctions api_{};
    ffxContext context_ = nullptr;
    Settings settings_{};
    Provider provider_{};
    // SDK creation-chain pointers remain valid for the entire context lifetime.
    ffxCreateContextDescFrameGeneration create_{};
    ffxCreateBackendVKDesc backend_{};
    ffxCreateContextDescFrameGenerationVersion version_{};
    ffxOverrideVersion override_{};
    bool history_ = false, faulted_ = false;
    uint64_t last_id_ = 0;
    FfxApiDimensions2D last_render_size_{};
    FfxApiRect2D last_rect_{};
};
} // namespace jgpu::fg
