#include "jgpu_frame_generation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jgpu::fg {
namespace {
bool size_ok(FfxApiDimensions2D s) {
    return s.width && s.height && s.width <= INT32_MAX && s.height <= INT32_MAX;
}
bool device_ok(VkDeviceContext d) {
    return d.vkDevice && d.vkPhysicalDevice && d.vkDeviceProcAddr;
}
bool color_format_ok(uint32_t f) {
    return f == FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM ||
           f == FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM ||
           f == FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT ||
           f == FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
}
bool image_ok(const FfxApiResource& r, FfxApiDimensions2D s) {
    return r.resource && r.description.type == FFX_API_RESOURCE_TYPE_TEXTURE2D &&
           r.description.width == s.width && r.description.height == s.height &&
           r.description.depth == 1 && r.description.mipCount == 1 &&
           r.description.format != FFX_API_SURFACE_FORMAT_UNKNOWN;
}
bool readable(const FfxApiResource& r) {
    return r.state == FFX_API_RESOURCE_STATE_COMPUTE_READ ||
           r.state == FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ ||
           r.state == FFX_API_RESOURCE_STATE_GENERIC_READ;
}
bool finite3(const float (&v)[3]) {
    return std::all_of(v, v + 3, [](float x) { return std::isfinite(x); });
}
float dot(const float (&a)[3], const float (&b)[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
bool basis_ok(const Frame& f) {
    return finite3(f.camera_position) && finite3(f.camera_up) &&
           finite3(f.camera_right) && finite3(f.camera_forward) &&
           std::abs(dot(f.camera_up, f.camera_up) - 1) < .01f &&
           std::abs(dot(f.camera_right, f.camera_right) - 1) < .01f &&
           std::abs(dot(f.camera_forward, f.camera_forward) - 1) < .01f &&
           std::abs(dot(f.camera_up, f.camera_right)) < .01f &&
           std::abs(dot(f.camera_up, f.camera_forward)) < .01f &&
           std::abs(dot(f.camera_right, f.camera_forward)) < .01f;
}
bool same(FfxApiDimensions2D a, FfxApiDimensions2D b) {
    return a.width == b.width && a.height == b.height;
}
bool same(FfxApiRect2D a, FfxApiRect2D b) {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
} // namespace

Result enumerate(const ffxFunctions& api, VkDeviceContext device, std::vector<Provider>& out) {
    out.clear();
    if (!api.Query || !device_ok(device)) return {Stage::invalid_input};
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    q.device = &device;
    q.outputCount = &count;
    auto rc = api.Query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) return {Stage::enumerate, rc};
    if (!count) return {Stage::unavailable};
    // Bound allocations even if a faulty provider returns an unreasonable count.
    if (count > 256) return {Stage::enumerate, FFX_API_RETURN_ERROR_PARAMETER};
    const auto capacity = count;
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count, nullptr);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    rc = api.Query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) return {Stage::enumerate, rc};
    if (count > capacity) return {Stage::enumerate, FFX_API_RETURN_ERROR_PARAMETER};
    for (uint64_t i = 0; i < count; ++i) {
        if (names[i] && *names[i]) out.push_back({ids[i], names[i]});
    }
    return out.empty() ? Result{Stage::unavailable} : Result{};
}

Context::~Context() { (void)close(); }

Result Context::open(const ffxFunctions& api, VkDeviceContext device, const Settings& s) {
    if (context_) return {Stage::already_open};
    constexpr uint32_t supported = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
        FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
        FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE |
        FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    if (!api.CreateContext || !api.DestroyContext || !api.Configure || !api.Query || !api.Dispatch ||
        !device_ok(device) || !size_ok(s.display_size) || !size_ok(s.max_render_size) ||
        s.max_render_size.width > s.display_size.width || s.max_render_size.height > s.display_size.height ||
        !color_format_ok(s.color_format) || (s.flags & ~supported)) return {Stage::invalid_input};
    std::vector<Provider> providers;
    auto result = enumerate(api, device, providers);
    if (!result) return result;
    const auto selected = std::find_if(providers.begin(), providers.end(),
        [&](const Provider& p) { return p.id == s.provider_id; });
    if (selected == providers.end()) return {Stage::unavailable};
    api_ = api;
    settings_ = s;
    provider_ = *selected;
    history_ = false;
    faulted_ = false;
    create_ = {};
    create_.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &backend_.header};
    create_.flags = s.flags;
    create_.displaySize = s.display_size;
    create_.maxRenderSize = s.max_render_size;
    create_.backBufferFormat = s.color_format;
    backend_ = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &version_.header}, device};
    version_ = {{FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &override_.header}, FFX_FRAMEGENERATION_VERSION};
    override_ = {{FFX_API_DESC_TYPE_OVERRIDE_VERSION, nullptr}, s.provider_id};
    auto rc = api_.CreateContext(&context_, &create_.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !context_) return fail(Stage::create, rc);
    ffxQueryGetProviderVersion actual{};
    actual.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    rc = api_.Query(&context_, &actual.header);
    if (rc != FFX_API_RETURN_OK || actual.versionId != s.provider_id || !actual.versionName || !*actual.versionName)
        return fail(Stage::verify_provider, rc);
    provider_.name = actual.versionName;
    return {};
}

bool Context::valid(const Frame& f) const {
    if (!f.commands || !size_ok(f.render_size) ||
        f.render_size.width > settings_.max_render_size.width || f.render_size.height > settings_.max_render_size.height)
        return false;
    const auto r = f.generation_rect;
    if (r.left < 0 || r.top < 0 || r.width <= 0 || r.height <= 0 ||
        uint64_t(r.left) + r.width > settings_.display_size.width ||
        uint64_t(r.top) + r.height > settings_.display_size.height) return false;
    // FSR requires matching content/depth aspect ratios. Do not interpolate a
    // stretched widescreen image using the original 16:9 geometry inputs.
    if (uint64_t(r.width) * f.render_size.height != uint64_t(r.height) * f.render_size.width) return false;
    const auto mv_size = (settings_.flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        ? settings_.display_size : f.render_size;
    if (!image_ok(f.color, settings_.display_size) || !image_ok(f.output, settings_.display_size) ||
        !image_ok(f.depth, f.render_size) || !image_ok(f.motion_vectors, mv_size) ||
        f.color.description.format != settings_.color_format || f.output.description.format != settings_.color_format ||
        !readable(f.color) || !readable(f.depth) || !readable(f.motion_vectors) ||
        f.output.state != FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ||
        !(f.output.description.usage & FFX_API_RESOURCE_USAGE_UAV)) return false;
    const auto df = f.depth.description.format, mf = f.motion_vectors.description.format;
    if ((df != FFX_API_SURFACE_FORMAT_R32_FLOAT && df != FFX_API_SURFACE_FORMAT_R16_UNORM && df != FFX_API_SURFACE_FORMAT_R16_FLOAT) ||
        (mf != FFX_API_SURFACE_FORMAT_R16G16_FLOAT && mf != FFX_API_SURFACE_FORMAT_R32G32_FLOAT)) return false;
    // No in-place operation or aliasing among the four image handles.
    const void* handles[] = {f.color.resource, f.depth.resource, f.motion_vectors.resource, f.output.resource};
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = i + 1; j < 4; ++j) if (handles[i] == handles[j]) return false;
    const float values[] = {f.elapsed_ms, f.camera_near, f.camera_far, f.vertical_fov_radians,
        f.view_space_to_meters, f.jitter_pixels.x, f.jitter_pixels.y, f.motion_vector_scale.x,
        f.motion_vector_scale.y, f.min_luminance, f.max_luminance};
    for (float v : values) if (!std::isfinite(v)) return false;
    if (f.elapsed_ms <= 0 || f.camera_near <= 0 || f.view_space_to_meters <= 0 ||
        f.vertical_fov_radians <= 0 || f.vertical_fov_radians >= 3.14159265f ||
        (!(settings_.flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) && f.camera_far <= f.camera_near) ||
        f.motion_vector_scale.x == 0 || f.motion_vector_scale.y == 0 || !basis_ok(f)) return false;
    const bool hdr = (settings_.flags & FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE) != 0;
    if (f.transfer_function > FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB ||
        hdr != (f.transfer_function != FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB)) return false;
    return !hdr || (f.min_luminance >= 0 && f.max_luminance > f.min_luminance);
}

Result Context::fail(Stage stage, ffxReturnCode_t rc) {
    history_ = false;
    faulted_ = true;
    return {stage, rc};
}

Result Context::record(const Frame& f) {
    if (faulted_) return {Stage::faulted};
    if (!context_) return {Stage::unavailable};
    if (!valid(f)) { history_ = false; return {Stage::invalid_input}; }
    const bool reset = f.reset || !history_ || last_id_ == std::numeric_limits<uint64_t>::max() ||
        f.id != last_id_ + 1 || !same(f.render_size, last_render_size_) || !same(f.generation_rect, last_rect_);
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.frameGenerationEnabled = true;
    config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    config.generationRect = f.generation_rect;
    config.frameID = f.id;
    auto rc = api_.Configure(&context_, &config.header);
    if (rc != FFX_API_RETURN_OK) return fail(Stage::configure, rc);
    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.commandList = f.commands;
    prep.frameID = f.id;
    prep.renderSize = f.render_size;
    prep.jitterOffset = f.jitter_pixels;
    prep.motionVectorScale = f.motion_vector_scale;
    prep.frameTimeDelta = f.elapsed_ms;
    prep.reset = reset;
    prep.cameraNear = f.camera_near;
    prep.cameraFar = f.camera_far;
    prep.cameraFovAngleVertical = f.vertical_fov_radians;
    prep.viewSpaceToMetersFactor = f.view_space_to_meters;
    prep.depth = f.depth;
    prep.motionVectors = f.motion_vectors;
    std::copy_n(f.camera_position, 3, prep.cameraPosition);
    std::copy_n(f.camera_up, 3, prep.cameraUp);
    std::copy_n(f.camera_right, 3, prep.cameraRight);
    std::copy_n(f.camera_forward, 3, prep.cameraForward);
    rc = api_.Dispatch(&context_, &prep.header);
    if (rc != FFX_API_RETURN_OK) return fail(Stage::prepare, rc);
    ffxDispatchDescFrameGeneration gen{};
    gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    gen.commandList = f.commands;
    gen.presentColor = f.color;
    gen.outputs[0] = f.output;
    gen.numGeneratedFrames = 1;
    gen.reset = reset;
    gen.backbufferTransferFunction = f.transfer_function;
    gen.minMaxLuminance[0] = f.min_luminance;
    gen.minMaxLuminance[1] = f.max_luminance;
    gen.generationRect = f.generation_rect;
    gen.frameID = f.id;
    rc = api_.Dispatch(&context_, &gen.header);
    if (rc != FFX_API_RETURN_OK) return fail(Stage::generate, rc);
    history_ = true;
    last_id_ = f.id;
    last_render_size_ = f.render_size;
    last_rect_ = f.generation_rect;
    return {Stage::ok, FFX_API_RETURN_OK, !reset};
}

Result Context::close() {
    history_ = false;
    if (context_) {
        const auto rc = api_.DestroyContext(&context_, nullptr);
        if (rc != FFX_API_RETURN_OK) return fail(Stage::destroy, rc);
    }
    context_ = nullptr;
    faulted_ = false;
    provider_ = {};
    return {};
}
} // namespace jgpu::fg
