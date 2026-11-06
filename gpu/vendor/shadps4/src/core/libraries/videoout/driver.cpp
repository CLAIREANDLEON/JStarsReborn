// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "core/platform.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

// jstars: low latency (early present, late frame start) and the timing of the path from the game's controller read to the
// picture. The explanation is at PresentThread.
namespace {

using SteadyClock = std::chrono::steady_clock;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}


// 0: off. 1: early present. 2: early present and late frame start. Fixed at start (JSTARS_LOW_LATENCY).
int LatencyLevel() {
    static const int level = EmulatorSettings.GetLowLatencyLevel();
    return level;
}

// The time of the last flip, and of the next vblank at which a flip can happen (published by the present thread).
std::atomic<s64> last_flip_ns{0};
std::atomic<s64> next_flip_ns{0};

// Input. The first controller read since the game's last frame submission (0: none yet) is the input the frame being built
// comes from. At the submission it moves, with the submission time, to the flip request the GPU thread queues for the frame.
std::atomic<s64> input_read_ns{0};
std::atomic<s64> submitted_input_ns{0};
std::atomic<s64> submitted_ns{0};
std::atomic<bool> frame_in_flight{false}; // between a frame's submission and its flip request
std::atomic<s64> last_request_ns{0};      // when the last flip request was queued

// Late frame start. The game starts a frame when the GPU thread reports the end of the previous one (the end-of-pipe
// interrupt). `build_budget_ns` is how long the game then needs until it submits the next frame: the slowest recent frame
// plus a margin (0: not measured yet). A deferred interrupt waits in `eop_pending` until `eop_due_ns`.
std::atomic<s64> build_budget_ns{0};
std::atomic<s64> eop_delivered_ns{0};
std::atomic<s64> eop_due_ns{0};
std::atomic<int> eop_pending{0};
std::atomic<u32> eop_deferred_count{0};

// The second largest of the newest values: one slow frame among them is not followed, a repeated one is.
s64 SecondLargest(const std::array<s64, 64>& values, size_t count) {
    s64 first = 0;
    s64 second = 0;
    const size_t used = std::min(count, values.size());
    for (size_t i = 0; i < used; ++i) {
        if (values[i] > first) {
            second = first;
            first = values[i];
        } else if (values[i] > second) {
            second = values[i];
        }
    }
    return used > 1 ? second : first;
}

void DeliverGfxEop() {
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
    eop_delivered_ns.store(NowNs(), std::memory_order_relaxed);
}

// Used by the present thread only.
struct PresentTiming {
    enum class Kind { Early, Late, AtFlip };
    static constexpr size_t Capacity = 512;

    std::array<float, Capacity> ready_ms{};
    std::array<float, Capacity> shown_ms{};
    std::array<float, Capacity> built_ms{};
    std::array<float, Capacity> input_ms{};
    size_t frames{};
    size_t inputs{};
    u32 early{};
    u32 late{};
    u32 at_flip{};
    s64 window_start_ns{};

    // ready and shown are measured from the last flip; built and input from the controller read the frame was built from.
    void Add(s64 ready_ns, s64 shown_ns, s64 input_ns, s64 submit_ns, Kind kind) {
        const s64 base_ns = last_flip_ns.load(std::memory_order_relaxed);
        if (base_ns == 0) {
            return; // no flip yet: nothing to measure against
        }
        if (frames < Capacity) {
            ready_ms[frames] = static_cast<float>(std::max<s64>(ready_ns - base_ns, 0)) * 1e-6f;
            shown_ms[frames] = static_cast<float>(shown_ns - base_ns) * 1e-6f;
            ++frames;
        }
        if (input_ns != 0 && submit_ns >= input_ns && inputs < Capacity) {
            built_ms[inputs] = static_cast<float>(submit_ns - input_ns) * 1e-6f;
            input_ms[inputs] = static_cast<float>(shown_ns - input_ns) * 1e-6f;
            ++inputs;
        }
        if (kind == Kind::Early) {
            ++early;
        } else if (kind == Kind::Late) {
            ++late;
        } else {
            ++at_flip;
        }
    }

    static float Pick(const std::array<float, Capacity>& sorted, size_t count, size_t percent) {
        return count ? sorted[std::min(count - 1, count * percent / 100)] : 0.0f;
    }

    // One log line every five seconds (runtime --frame-stats).
    void Report(double lead_ms, double slot_ms) {
        const s64 now_ns = NowNs();
        if (window_start_ns == 0) {
            window_start_ns = now_ns;
        }
        if (now_ns - window_start_ns < 5000000000LL) {
            return;
        }
        if (frames != 0) {
            std::sort(ready_ms.begin(), ready_ms.begin() + frames);
            std::sort(shown_ms.begin(), shown_ms.begin() + frames);
            std::sort(built_ms.begin(), built_ms.begin() + inputs);
            std::sort(input_ms.begin(), input_ms.begin() + inputs);
            LOG_INFO(Lib_VideoOut,
                     "present timing: {} frames in {:.1f} s, a flip every {:.1f} ms; frame ready {:.1f} ms "
                     "after the previous flip (p95 {:.1f}, max {:.1f}); shown after {:.1f} ms (p95 {:.1f}, "
                     "max {:.1f}); controller read to frame submitted {:.1f} ms (p95 {:.1f}, max {:.1f}); "
                     "controller read to picture {:.1f} ms (p95 {:.1f}, max {:.1f}; {} frames); {} shown "
                     "early, {} late, {} at the flip; low latency level {} (lead {:.1f} ms, frame start "
                     "{:.1f} ms before the flip, {} delayed)",
                     frames, static_cast<double>(now_ns - window_start_ns) * 1e-9, slot_ms,
                     Pick(ready_ms, frames, 50), Pick(ready_ms, frames, 95),
                     Pick(ready_ms, frames, 100), Pick(shown_ms, frames, 50),
                     Pick(shown_ms, frames, 95), Pick(shown_ms, frames, 100),
                     Pick(built_ms, inputs, 50), Pick(built_ms, inputs, 95),
                     Pick(built_ms, inputs, 100), Pick(input_ms, inputs, 50),
                     Pick(input_ms, inputs, 95), Pick(input_ms, inputs, 100), inputs, early, late,
                     at_flip, LatencyLevel(), lead_ms,
                     static_cast<double>(build_budget_ns.load(std::memory_order_relaxed)) * 1e-6,
                     eop_deferred_count.exchange(0, std::memory_order_relaxed));
        }
        frames = inputs = 0;
        early = late = at_flip = 0;
        window_start_ns = now_ns;
    }
};
PresentTiming timing;

} // namespace

void NoteInputRead() {
    s64 expected = 0;
    input_read_ns.compare_exchange_strong(expected, NowNs(), std::memory_order_relaxed);
}

void NoteFrameSubmit() {
    const s64 now_ns = NowNs();
    submitted_input_ns.store(input_read_ns.exchange(0, std::memory_order_relaxed),
                             std::memory_order_relaxed);
    submitted_ns.store(now_ns, std::memory_order_relaxed);
    frame_in_flight.store(true, std::memory_order_relaxed);

    // How long the game took from the interrupt that started this frame to its submission (the game submits from one
    // thread). The budget follows the slow frames of the last 64, so a slow stretch is covered from its second frame on.
    constexpr s64 BuildMarginNs = 2000000;
    static std::array<s64, 64> recent{};
    static size_t count = 0;
    const s64 woke_ns = eop_delivered_ns.exchange(0, std::memory_order_relaxed);
    if (woke_ns != 0 && now_ns - woke_ns < 100000000) {
        recent[count++ % recent.size()] = now_ns - woke_ns;
        if (count >= 16) {
            build_budget_ns.store(SecondLargest(recent, count) + BuildMarginNs,
                                  std::memory_order_relaxed);
        }
    }
}

void SignalGfxEop() {
    const s64 budget_ns = build_budget_ns.load(std::memory_order_relaxed);
    if (LatencyLevel() < 2 || budget_ns == 0) {
        DeliverGfxEop();
        return;
    }
    // Late frame start: the GPU thread cannot begin the game's next frame before the next flip (it waits for the display
    // buffer), so waking the game earlier than it needs to make that flip only makes the frame's input older. Only the
    // interrupt that ends a frame is held back (the frame's flip request is in flight or was just queued), and only in
    // the regular frame loop (a recent flip, and still time before the next one); any other is delivered at once.
    const s64 now_ns = NowNs();
    const s64 due_ns = next_flip_ns.load(std::memory_order_relaxed) - budget_ns;
    const bool ends_frame = frame_in_flight.load(std::memory_order_relaxed) ||
                            now_ns - last_request_ns.load(std::memory_order_relaxed) < 2000000;
    if (!ends_frame || due_ns <= now_ns ||
        now_ns - last_flip_ns.load(std::memory_order_relaxed) > 100000000) {
        DeliverGfxEop();
        return;
    }
    eop_due_ns.store(due_ns, std::memory_order_relaxed);
    eop_pending.fetch_add(1, std::memory_order_relaxed);
    eop_deferred_count.fetch_add(1, std::memory_order_relaxed);
}

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

void VideoOutDriver::Flip(const Request& req) {
    if (!req.presented) { // jstars: unless the early present already showed it (PresentThread)
        // Update HDR status before presenting.
        presenter->SetHDR(req.port->is_hdr);

        // Present the frame.
        presenter->Present(req.frame);
        timing.Add(req.ready_ns, NowNs(), req.input_ns, req.submit_ns,
                   PresentTiming::Kind::AtFlip);
    }

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    last_flip_ns.store(NowNs(), std::memory_order_relaxed); // jstars

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(true);
    presenter->Present(empty_frame, false, false);
}

void VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    std::scoped_lock lock{mutex};
    requests.push({
        .frame = frame,
        .port = port,
        .flip_arg = flip_arg,
        .index = index,
        .eop = is_eop,
        .presented = false,
        .ready_ns = NowNs(),
        .input_ns = submitted_input_ns.exchange(0, std::memory_order_relaxed),
        .submit_ns = submitted_ns.load(std::memory_order_relaxed),
    });
    // jstars: the present thread may show the frame before its flip
    frame_in_flight.store(false, std::memory_order_relaxed);
    last_request_ns.store(NowNs(), std::memory_order_relaxed);
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    // jstars: low latency. The game runs its frames like this: when the GPU thread reports the end of a frame (the
    // end-of-pipe interrupt), the game reads the controller, builds the next frame and submits it; the GPU thread waits
    // for the flip of the frame before it (which frees the display buffer), renders, and queues the flip request; the
    // request is flipped at the next vblank that takes one. With the game's flip rate 1 (30 fps) the picture therefore
    // reached the screen about 62 ms after the controller read it was built from: the frame waited ~29 ms for the display
    // buffer and, once rendered, ~29 ms more for its flip.
    //   Level 1, early present: a queued frame is presented `lead` after the last flip instead of at its own flip.
    //   Level 2, late frame start: in addition the end-of-pipe interrupt is delivered only as late as the game still
    //   needs to submit its next frame before the coming flip (SignalGfxEop), so the controller is read just before
    //   that flip instead of right after the previous one.
    // Nothing the game can see changes except when its GPU work appears to finish: flips (status and events) stay on their
    // vblanks in the submitted order, and every frame is built, rendered and flipped as before. `lead` follows the slow
    // frames of the last 64 plus a margin, which keeps the cadence even; a frame that takes longer is shown the moment it
    // is ready. JSTARS_LOW_LATENCY=0|1|2 (runtime --low-latency), default 2.
    const int latency_level = LatencyLevel();
    const bool frame_stats = EmulatorSettings.IsFrameStatsEnabled();
    constexpr s64 LeadMarginNs = 1500000; // on top of the slowest recent frame
    constexpr s64 LeadDecayNs = 250000;   // per frame, towards a lower target
    constexpr s64 GuardNs = 2000000;      // the end of every vblank wait is left to the timer
    constexpr s64 PollNs = 1000000;       // longest sleep before looking for a new frame or interrupt again
    std::array<s64, 64> recent_ready{};
    size_t recent_count = 0;
    s64 lead_ns = 0;
    const auto slot_ns = [&]() -> s64 { return vblank_period.count() * (main_port.flip_rate + 1); };
    // Call before the request's own flip: the offset is measured from the flip before it.
    const auto note_ready = [&](s64 ready_ns) {
        const s64 offset =
            std::max<s64>(ready_ns - last_flip_ns.load(std::memory_order_relaxed), 0);
        if (offset >= slot_ns() - GuardNs) {
            return; // missed its flip: says nothing about a normal frame
        }
        recent_ready[recent_count++ % recent_ready.size()] = offset;
        const s64 target = SecondLargest(recent_ready, recent_count) + LeadMarginNs;
        lead_ns = target >= lead_ns ? target : std::max(target, lead_ns - LeadDecayNs);
    };
    const auto deliver_due_interrupts = [&](s64 until_ns) {
        if (eop_pending.load(std::memory_order_relaxed) > 0 &&
            eop_due_ns.load(std::memory_order_relaxed) <= until_ns) {
            for (int n = eop_pending.exchange(0, std::memory_order_relaxed); n > 0; --n) {
                DeliverGfxEop();
            }
        }
    };
    // The wait for the next vblank, during which a finished frame is shown and a deferred interrupt delivered when due.
    const auto wait_for_vblank = [&](s64 next_vblank_ns) {
        const s64 limit_ns = next_vblank_ns - GuardNs;
        while (!token.stop_requested()) {
            const s64 now_ns = NowNs();
            if (now_ns >= limit_ns) {
                break;
            }
            s64 next_ns = limit_ns;
            if (eop_pending.load(std::memory_order_relaxed) > 0) {
                const s64 due_ns = eop_due_ns.load(std::memory_order_relaxed);
                if (due_ns <= now_ns) {
                    deliver_due_interrupts(now_ns);
                    continue;
                }
                next_ns = std::min(next_ns, due_ns);
            }
            Request head{};
            {
                std::scoped_lock lock{mutex};
                if (!requests.empty() && !requests.front().presented) {
                    head = requests.front();
                }
            }
            if (head) {
                // A frame that was ready before the last flip has waited long enough; the others get an even cadence.
                const s64 flip_ns = last_flip_ns.load(std::memory_order_relaxed);
                const s64 due_ns = head.ready_ns <= flip_ns ? now_ns : flip_ns + lead_ns;
                if (due_ns <= now_ns) {
                    presenter->SetHDR(head.port->is_hdr);
                    presenter->Present(head.frame);
                    timing.Add(head.ready_ns, NowNs(), head.input_ns, head.submit_ns,
                               head.ready_ns <= due_ns ? PresentTiming::Kind::Early
                                                       : PresentTiming::Kind::Late);
                    note_ready(head.ready_ns);
                    std::scoped_lock lock{mutex};
                    requests.front().presented = true; // only this thread removes requests
                    continue;
                }
                if (due_ns <= limit_ns) {
                    next_ns = std::min(next_ns, due_ns);
                } // else: it is flipped at the next vblank, or shown in the wait after it
            }
            // Sleep until the next thing is due, in short steps because a frame or an interrupt may arrive meanwhile.
            // (A timed wait on a condition variable could be woken for those, but its timeout is only as fine as the
            // timer resolution Windows grants the process, and with the game window in the background such waits
            // ended about 10 ms late in a test run.)
            Common::AccurateSleep(std::chrono::nanoseconds(std::min(next_ns - now_ns, PollNs)),
                                  nullptr, false);
        }
        deliver_due_interrupts(next_vblank_ns); // due within the guard: a little early rather than after the vblank
    };

    while (!token.stop_requested()) {
        timer.Start();
        const s64 vblank_ns = NowNs(); // jstars
        deliver_due_interrupts(vblank_ns);

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        if (vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                if (latency_level > 0 && !request.presented) {
                    note_ready(request.ready_ns); // jstars
                }
                Flip(request);
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        // jstars: spend the wait for the next vblank where a finished frame can be shown and a deferred interrupt
        // delivered (see above); the timer sleeps whatever is left. The next vblank is one period after this one,
        // corrected by what the timer still owes; a flip can happen at the first vblank whose count is a multiple of
        // the flip rate + 1.
        {
            const s64 next_vblank_ns =
                vblank_ns + vblank_period.count() + timer.GetTotalWait().count();
            const u64 every = static_cast<u64>(main_port.flip_rate) + 1;
            const u64 skipped = (every - vblank_status.count % every) % every;
            next_flip_ns.store(next_vblank_ns + static_cast<s64>(skipped) * vblank_period.count(),
                               std::memory_order_relaxed);
            if (latency_level > 0) {
                wait_for_vblank(next_vblank_ns);
            }
        }
        if (frame_stats) {
            timing.Report(static_cast<double>(lead_ns) * 1e-6,
                          static_cast<double>(slot_ns()) * 1e-6);
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut
