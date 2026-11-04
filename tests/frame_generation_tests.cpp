// CPU-only SDK contract tests. Never loads a DLL, accesses a GPU or starts the game.
#include "jgpu_frame_generation.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace jgpu::fg;
namespace {
int checks = 0;
void check(bool value, const char* what) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
struct Mock {
    std::string calls;
    Stage failure = Stage::ok;
    bool mismatch = false, no_providers = false, huge_count = false, grow_count = false;
    ffxCreateContextDescHeader* retained_create = nullptr;
    ffxConfigureDescFrameGeneration config{};
    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    ffxDispatchDescFrameGeneration gen{};
} mock;
constexpr uint64_t selected_id = 0x11223344;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice, const char*) { return nullptr; }
VkDeviceContext device() {
    return {reinterpret_cast<VkDevice>(uintptr_t(1)), reinterpret_cast<VkPhysicalDevice>(uintptr_t(2)), device_proc};
}
ffxReturnCode_t query(ffxContext* ctx, ffxQueryDescHeader* header) {
    if (!ctx) {
        check(header->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, "enumeration descriptor");
        auto& q = *reinterpret_cast<ffxQueryDescGetVersions*>(header);
        check(q.createDescType == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION && q.device, "FG query uses Vulkan device");
        if (mock.failure == Stage::enumerate) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        if (q.versionIds) {
            check(*q.outputCount == 2, "enumeration supplies array capacity");
            q.versionIds[0] = 17; q.versionNames[0] = "FSR 3.1.6";
            q.versionIds[1] = selected_id; q.versionNames[1] = "FSR 4.0.2";
            *q.outputCount = mock.grow_count ? 3 : 2;
        } else *q.outputCount = mock.no_providers ? 0 : mock.huge_count ? 257 : 2;
    } else {
        check(header->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, "actual provider queried after create");
        if (mock.failure == Stage::verify_provider) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        auto& q = *reinterpret_cast<ffxQueryGetProviderVersion*>(header);
        q.versionId = mock.mismatch ? 17 : selected_id;
        q.versionName = mock.mismatch ? "FSR 3.1.6" : "FSR 4.0.2";
    }
    return FFX_API_RETURN_OK;
}
void inspect_chain(ffxCreateContextDescHeader* header) {
    check(header->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, "FG create descriptor");
    auto* backend = reinterpret_cast<ffxCreateBackendVKDesc*>(header->pNext);
    check(backend && backend->header.type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK &&
          backend->deviceContext.vkDevice == device().vkDevice, "Vulkan backend chained");
    auto* version = reinterpret_cast<ffxCreateContextDescFrameGenerationVersion*>(backend->header.pNext);
    check(version && version->header.type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION &&
          version->version == FFX_FRAMEGENERATION_VERSION, "PrepareV2 API version opt-in");
    auto* override = reinterpret_cast<ffxOverrideVersion*>(version->header.pNext);
    check(override && override->header.type == FFX_API_DESC_TYPE_OVERRIDE_VERSION &&
          override->versionId == selected_id && !override->header.pNext, "explicit provider override, not default FSR3");
}
ffxReturnCode_t create(ffxContext* ctx, ffxCreateContextDescHeader* header, const ffxAllocationCallbacks*) {
    mock.calls += 'C';
    inspect_chain(header);
    mock.retained_create = header;
    *ctx = reinterpret_cast<void*>(uintptr_t(3));
    return mock.failure == Stage::create ? FFX_API_RETURN_ERROR_MEMORY : FFX_API_RETURN_OK;
}
ffxReturnCode_t destroy(ffxContext* ctx, const ffxAllocationCallbacks*) {
    mock.calls += 'D';
    inspect_chain(mock.retained_create); // Would fail if open() built a temporary chain.
    if (mock.failure == Stage::destroy) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    *ctx = nullptr;
    return FFX_API_RETURN_OK;
}
ffxReturnCode_t configure(ffxContext*, const ffxConfigureDescHeader* header) {
    mock.calls += 'c';
    check(header->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION, "configure descriptor");
    mock.config = *reinterpret_cast<const ffxConfigureDescFrameGeneration*>(header);
    return mock.failure == Stage::configure ? FFX_API_RETURN_ERROR_RUNTIME_ERROR : FFX_API_RETURN_OK;
}
ffxReturnCode_t dispatch(ffxContext*, const ffxDispatchDescHeader* header) {
    if (header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2) {
        mock.calls += 'p';
        mock.prep = *reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2*>(header);
        return mock.failure == Stage::prepare ? FFX_API_RETURN_ERROR_RUNTIME_ERROR : FFX_API_RETURN_OK;
    }
    check(header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION, "generate descriptor");
    mock.calls += 'g';
    mock.gen = *reinterpret_cast<const ffxDispatchDescFrameGeneration*>(header);
    return mock.failure == Stage::generate ? FFX_API_RETURN_ERROR_RUNTIME_ERROR : FFX_API_RETURN_OK;
}
ffxFunctions api() { return {create, destroy, configure, query, dispatch}; }
Settings settings() {
    return {selected_id, {1920, 1080}, {1280, 720}, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, 0};
}
FfxApiResource image(uintptr_t handle, uint32_t w, uint32_t h, uint32_t format, bool output = false) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void*>(handle);
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.width = w; r.description.height = h;
    r.description.depth = r.description.mipCount = 1;
    r.description.format = format;
    r.description.usage = output ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}
Frame frame(uint64_t id = 10) {
    Frame f{};
    f.commands = reinterpret_cast<VkCommandBuffer>(uintptr_t(4));
    f.id = id; f.render_size = {1280, 720}; f.generation_rect = {0, 0, 1920, 1080};
    f.color = image(10, 1920, 1080, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM);
    f.output = image(11, 1920, 1080, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, true);
    f.depth = image(12, 1280, 720, FFX_API_SURFACE_FORMAT_R32_FLOAT);
    f.motion_vectors = image(13, 1280, 720, FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
    f.motion_vector_scale = {1280, -720}; // Sign is caller's verified convention.
    f.jitter_pixels = {.25f, -.25f};
    f.elapsed_ms = 1000.f / 30;
    f.camera_near = .1f; f.camera_far = 1000; f.vertical_fov_radians = 1;
    f.view_space_to_meters = .01f;
    f.camera_position[0] = 12; f.camera_position[1] = 3;
    f.camera_up[1] = f.camera_right[0] = f.camera_forward[2] = 1;
    return f;
}
void successful_recording() {
    mock = {};
    Context c;
    check(bool(c.open(api(), device(), settings())), "open");
    check(c.provider().id == selected_id && c.provider().name == "FSR 4.0.2", "verified provider exposed");
    check(c.open(api(), device(), settings()).stage == Stage::already_open, "no overwriting a live context");
    mock.calls.clear();
    auto f = frame();
    auto result = c.record(f);
    check(bool(result) && !result.interpolated && mock.calls == "cpg", "first frame seeds history in SDK-required order");
    check(mock.prep.reset && mock.gen.reset && mock.config.frameID == f.id &&
          mock.prep.frameID == f.id && mock.gen.frameID == f.id, "reset and same real-frame ID in all three calls");
    check(mock.config.frameGenerationEnabled && !mock.config.allowAsyncWorkloads && !mock.config.swapChain &&
          mock.config.flags == FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY, "serial caller-managed presentation");
    check(mock.prep.frameTimeDelta == f.elapsed_ms && mock.prep.jitterOffset.x == .25f &&
          mock.prep.motionVectorScale.y == -720 && mock.prep.cameraPosition[0] == 12 &&
          mock.prep.cameraUp[1] == 1 && mock.prep.cameraFovAngleVertical == 1 &&
          mock.prep.depth.resource == f.depth.resource && mock.prep.motionVectors.resource == f.motion_vectors.resource,
          "actual timing, camera and geometry forwarded unchanged");
    check(mock.gen.numGeneratedFrames == 1 && mock.gen.outputs[0].resource == f.output.resource &&
          !mock.gen.outputs[1].resource && mock.gen.presentColor.resource == f.color.resource &&
          mock.gen.commandList == f.commands && mock.prep.commandList == f.commands, "one separate output, one command buffer");
    f.id++;
    check(c.record(f).interpolated && !mock.prep.reset, "next consecutive frame can interpolate");
    f.id += 2;
    check(!c.record(f).interpolated && mock.prep.reset, "dropped rendered frame resets");
    f.id++;
    check(c.record(f).interpolated, "history recovers after gap");
    c.reset_history(); f.id++;
    check(!c.record(f).interpolated, "bypass/abandoned commands reset explicitly");
    f.id++; f.reset = true;
    check(!c.record(f).interpolated, "camera cut resets");
    f.reset = false; f.id++;
    f.render_size = {640, 360}; f.depth.description.width = f.motion_vectors.description.width = 640;
    f.depth.description.height = f.motion_vectors.description.height = 360;
    check(bool(c.record(f)) && mock.prep.reset, "render size change resets");
    f = frame(std::numeric_limits<uint64_t>::max());
    check(bool(c.record(f)), "maximum frame ID accepted with reset");
    f.id = 0;
    check(!c.record(f).interpolated && mock.prep.reset, "ID wrap resets");
    check(bool(c.close()) && bool(c.close()), "explicit close is idempotent");
}
void invalid_inputs() {
    mock = {}; Context c;
    check(bool(c.open(api(), device(), settings())), "open for validation");
    check(bool(c.record(frame(8))) && c.record(frame(9)).interpolated, "seed valid history before rejected frames");
    auto invalid = [&](Frame f, const char* why) {
        mock.calls.clear();
        auto r = c.record(f);
        check(r.stage == Stage::invalid_input && !r.interpolated && mock.calls.empty(), why);
    };
    auto f = frame(); f.depth.resource = nullptr; invalid(f, "missing depth does not call SDK");
    f = frame(); f.motion_vectors.resource = nullptr; invalid(f, "missing vectors does not call SDK");
    f = frame(); f.camera_forward[2] = 0; invalid(f, "zero camera basis rejected");
    f = frame(); f.camera_up[1] = 0; f.camera_up[0] = 1; invalid(f, "parallel camera axes rejected");
    f = frame(); f.elapsed_ms = std::numeric_limits<float>::quiet_NaN(); invalid(f, "NaN timing rejected");
    f = frame(); f.elapsed_ms = 0; invalid(f, "zero elapsed time rejected");
    f = frame(); f.vertical_fov_radians = 60; invalid(f, "degrees must not be passed as radians");
    f = frame(); f.motion_vector_scale = {}; invalid(f, "unspecified vector convention rejected");
    f = frame(); f.output.resource = f.color.resource; invalid(f, "in-place output rejected");
    f = frame(); f.color.state = FFX_API_RESOURCE_STATE_PRESENT; invalid(f, "present layout not treated as shader-readable");
    f = frame(); f.color.state |= FFX_API_RESOURCE_STATE_RENDER_TARGET; invalid(f, "contradictory read/write state rejected");
    f = frame(); f.output.description.usage = 0; invalid(f, "output needs storage usage");
    f = frame(); f.output.description.width--; invalid(f, "output dimensions checked");
    f = frame(); f.motion_vectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_UINT; invalid(f, "integer vectors rejected");
    f = frame(); f.generation_rect.width = 2000; invalid(f, "rectangle bounds checked");
    f = frame(); f.generation_rect.width = 1600; invalid(f, "stretched geometry aspect rejected");
    f = frame(); f.generation_rect.left = -1; invalid(f, "negative rectangle rejected");
    f = frame(); f.transfer_function = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ; invalid(f, "HDR requires matching context");
    f = frame();
    check(!c.record(f).interpolated, "valid frame following rejected input resets");
}
void alternate_inputs() {
    mock = {};
    auto s = settings();
    s.flags = FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE |
        FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
        FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    s.color_format = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    Context c;
    check(bool(c.open(api(), device(), s)), "HDR and depth/vector convention flags accepted");
    check(reinterpret_cast<ffxCreateContextDescFrameGeneration*>(mock.retained_create)->flags == s.flags,
          "context convention flags preserved");
    auto f = frame();
    f.color.description.format = f.output.description.format = s.color_format;
    f.transfer_function = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    f.min_luminance = .01f; f.max_luminance = 1000;
    f.camera_far = 0; // Ignored in infinite-depth mode.
    f.motion_vectors.description.width = 1920; f.motion_vectors.description.height = 1080;
    f.motion_vector_scale = {1920, -1080};
    check(bool(c.record(f)) && mock.gen.minMaxLuminance[0] == .01f &&
          mock.gen.minMaxLuminance[1] == 1000 && mock.prep.cameraFar == 0 &&
          mock.prep.motionVectorScale.x == 1920, "HDR luminance and display vectors forwarded");
    f.max_luminance = f.min_luminance;
    check(c.record(f).stage == Stage::invalid_input, "invalid HDR luminance range rejected");
    check(bool(c.close()), "close HDR context");
    check(bool(c.open(api(), device(), settings())), "open SDR context");
    f = frame(); f.generation_rect = {320, 180, 1280, 720};
    check(bool(c.record(f)), "composited content with matching depth aspect accepted");
    f.id++; f.generation_rect.left = 300;
    check(bool(c.record(f)) && mock.gen.reset, "moving content rectangle resets history");
}
void failures() {
    for (Stage stage : {Stage::configure, Stage::prepare, Stage::generate}) {
        mock = {}; Context c;
        check(bool(c.open(api(), device(), settings())), "open before dispatch failure");
        mock.calls.clear(); mock.failure = stage;
        const auto r = c.record(frame());
        check(r.stage == stage && r.sdk == FFX_API_RETURN_ERROR_RUNTIME_ERROR && !r.interpolated, "SDK failure preserved");
        check(mock.calls == (stage == Stage::configure ? "c" : stage == Stage::prepare ? "cp" : "cpg"), "stop immediately at failed stage");
        mock.calls.clear(); mock.failure = Stage::ok; c.reset_history();
        check(c.record(frame(11)).stage == Stage::faulted && mock.calls.empty(), "failed context cannot be reused by reset");
        check(bool(c.close()) && bool(c.open(api(), device(), settings())), "close and recreate recovers");
        check(!c.record(frame(12)).interpolated, "recreated context seeds history");
    }
    for (Stage stage : {Stage::create, Stage::verify_provider}) {
        mock = {}; mock.failure = stage; Context c;
        check(c.open(api(), device(), settings()).stage == stage, "creation failure surfaced");
        check(c.record(frame()).stage == Stage::faulted, "partial context cannot record");
        mock.failure = Stage::ok;
        check(bool(c.close()) && mock.calls == "CD", "partial context cleaned up");
    }
    mock = {}; mock.mismatch = true;
    { Context c; check(c.open(api(), device(), settings()).stage == Stage::verify_provider, "silent provider fallback refused"); }
    mock = {}; Context c;
    check(bool(c.open(api(), device(), settings())), "open before destroy failure");
    mock.failure = Stage::destroy;
    check(c.close().stage == Stage::destroy && c.record(frame()).stage == Stage::faulted, "destroy error retained");
    mock.failure = Stage::ok;
    check(bool(c.close()), "destroy can be retried before DLL unload");
}
void selection() {
    mock = {}; std::vector<Provider> p;
    check(bool(enumerate(api(), device(), p)) && p.size() == 2 && p[0].id == 17 && p[1].id == selected_id, "opaque IDs enumerated");
    mock.no_providers = true;
    check(enumerate(api(), device(), p).stage == Stage::unavailable && p.empty(), "unavailable hardware is a clean bypass");
    mock = {}; mock.huge_count = true;
    check(enumerate(api(), device(), p).stage == Stage::enumerate, "unbounded provider count rejected");
    mock = {}; mock.grow_count = true;
    check(enumerate(api(), device(), p).stage == Stage::enumerate && p.empty(), "changed provider count rejected");
    mock = {}; mock.failure = Stage::enumerate;
    check(enumerate(api(), device(), p).sdk == FFX_API_RETURN_ERROR_RUNTIME_ERROR, "query error surfaced");
    mock = {}; Context c; auto s = settings(); s.provider_id = 99;
    check(c.open(api(), device(), s).stage == Stage::unavailable && mock.calls.empty(), "unknown ID does not fall back");
    s = settings(); s.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    check(c.open(api(), device(), s).stage == Stage::invalid_input, "async not enabled without synchronization");
    auto incomplete = api(); incomplete.Dispatch = nullptr;
    check(c.open(incomplete, device(), settings()).stage == Stage::invalid_input, "missing SDK export rejected");
    check(c.record(frame()).stage == Stage::unavailable, "closed context bypasses");
}
} // namespace
int main() {
    selection(); successful_recording(); invalid_inputs(); alternate_inputs(); failures();
    std::printf("frame_generation_tests: %d checks passed (CPU mocks; no GPU validation)\n", checks);
}
