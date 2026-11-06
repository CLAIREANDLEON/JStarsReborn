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
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

// jstars: early present and the timing of the path from the game's input read to the picture. See PresentThread.
namespace {

using SteadyClock = std::chrono::steady_clock;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}

// The time of the last flip, which releases the game's frame loop, and of the first controller read after it (0: none
// yet): the input the frame being rendered is built from.
std::atomic<s64> last_flip_ns{0};
std::atomic<s64> input_read_ns{0};

// Used by the present thread only.
struct PresentTiming {
    enum class Kind { Early, Late, AtFlip };
    static constexpr size_t Capacity = 512;

    std::array<float, Capacity> ready_ms{};
    std::array<float, Capacity> shown_ms{};
    std::array<float, Capacity> input_ms{};
    size_t frames{};
    size_t inputs{};
    u32 early{};
    u32 late{};
    u32 at_flip{};
    s64 window_start_ns{};

    void Add(s64 ready_ns, s64 shown_ns, Kind kind) {
        const s64 base_ns = last_flip_ns.load(std::memory_order_relaxed);
        if (base_ns == 0) {
            return; // no flip yet: nothing to measure against
        }
        if (frames < Capacity) {
            ready_ms[frames] = static_cast<float>(std::max<s64>(ready_ns - base_ns, 0)) * 1e-6f;
            shown_ms[frames] = static_cast<float>(shown_ns - base_ns) * 1e-6f;
            ++frames;
        }
        const s64 read_ns = input_read_ns.load(std::memory_order_relaxed);
        if (read_ns != 0 && inputs < Capacity) {
            input_ms[inputs++] = static_cast<float>(shown_ns - read_ns) * 1e-6f;
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
    void Report(bool early_present, double lead_ms, double slot_ms) {
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
            std::sort(input_ms.begin(), input_ms.begin() + inputs);
            LOG_INFO(Lib_VideoOut,
                     "present timing: {} frames in {:.1f} s, a flip every {:.1f} ms; frame ready {:.1f} ms "
                     "after the previous flip (p95 {:.1f}, max {:.1f}); shown after {:.1f} ms (p95 {:.1f}, "
                     "max {:.1f}); input read to picture {:.1f} ms (p95 {:.1f}, max {:.1f}; {} frames); {} "
                     "shown early, {} late, {} at the flip; early present {} (lead {:.1f} ms)",
                     frames, static_cast<double>(now_ns - window_start_ns) * 1e-9, slot_ms,
                     Pick(ready_ms, frames, 50), Pick(ready_ms, frames, 95),
                     Pick(ready_ms, frames, 100), Pick(shown_ms, frames, 50),
                     Pick(shown_ms, frames, 95), Pick(shown_ms, frames, 100),
                     Pick(input_ms, inputs, 50), Pick(input_ms, inputs, 95),
                     Pick(input_ms, inputs, 100), inputs, early, late, at_flip,
                     early_present ? "on" : "off", lead_ms);
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
        timing.Add(req.ready_ns, NowNs(), PresentTiming::Kind::AtFlip);
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

    // jstars: the game's next frame starts with this flip.
    input_read_ns.store(0, std::memory_order_relaxed);
    last_flip_ns.store(NowNs(), std::memory_order_relaxed);

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
    });
    request_cv.notify_one(); // jstars: the present thread may show it before its flip
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

    // jstars: early present. The game starts a frame when a flip completes, and the finished frame used to wait for the
    // next flip (33 ms later at the game's 30 fps) before it was shown, although it is ready long before that. Here a
    // queued frame is presented `lead` after the flip that started it, between two vblanks. The flip itself (status and
    // events: everything the game can see) still happens at its vblank, so the game runs exactly as before; only the
    // picture arrives sooner. `lead` follows the slowest recent frame plus a margin, which keeps the cadence even; a
    // frame that takes longer is shown the moment it is ready. Off with JSTARS_EARLY_PRESENT=0 (--no-early-present).
    const bool early_present = EmulatorSettings.IsEarlyPresentEnabled();
    const bool frame_stats = EmulatorSettings.IsFrameStatsEnabled();
    constexpr s64 LeadMarginNs = 1500000; // on top of the slowest recent frame
    constexpr s64 LeadDecayNs = 250000;   // per frame, towards a lower target
    constexpr s64 GuardNs = 2000000;      // the end of every vblank wait is left to the timer
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
        const auto end = recent_ready.begin() + std::min(recent_count, recent_ready.size());
        const s64 target = *std::max_element(recent_ready.begin(), end) + LeadMarginNs;
        lead_ns = target >= lead_ns ? target : std::max(target, lead_ns - LeadDecayNs);
    };
    const auto present_early = [&](SteadyClock::time_point next_vblank) {
        const auto limit = next_vblank - std::chrono::nanoseconds(GuardNs);
        std::unique_lock lock{mutex};
        while (!token.stop_requested() && SteadyClock::now() < limit) {
            if (requests.empty() || requests.front().presented) {
                request_cv.wait_until(lock, limit);
                continue;
            }
            const s64 due_ns = last_flip_ns.load(std::memory_order_relaxed) + lead_ns;
            const SteadyClock::time_point due{std::chrono::duration_cast<SteadyClock::duration>(
                std::chrono::nanoseconds(due_ns))};
            if (due > limit) {
                break; // not before the next vblank: it is flipped there, or shown in the wait after it
            }
            Vulkan::Frame* const frame = requests.front().frame;
            const bool is_hdr = requests.front().port->is_hdr;
            const s64 ready_ns = requests.front().ready_ns;
            lock.unlock();
            if (const auto now = SteadyClock::now(); due > now) {
                Common::AccurateSleep(due - now, nullptr, false);
            }
            presenter->SetHDR(is_hdr);
            presenter->Present(frame);
            timing.Add(ready_ns, NowNs(),
                       ready_ns <= due_ns ? PresentTiming::Kind::Early : PresentTiming::Kind::Late);
            note_ready(ready_ns);
            lock.lock();
            requests.front().presented = true; // only this thread removes requests
        }
    };

    while (!token.stop_requested()) {
        timer.Start();
        const auto vblank_time = SteadyClock::now(); // jstars

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
                if (early_present && !request.presented) {
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

        // jstars: spend the wait for the next vblank here, where a finished frame can be shown before it (see above);
        // the timer sleeps whatever is left. The next vblank is one period after this one, corrected by what the timer
        // still owes.
        if (early_present) {
            present_early(vblank_time + vblank_period + timer.GetTotalWait());
        }
        if (frame_stats) {
            timing.Report(early_present, static_cast<double>(lead_ns) * 1e-6,
                          static_cast<double>(slot_ns()) * 1e-6);
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut
