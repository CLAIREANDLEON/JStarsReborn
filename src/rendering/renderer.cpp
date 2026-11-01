#include "reborn/renderer.hpp"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace reborn {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT buffer_count = 2;
constexpr auto gpu_timeout = std::chrono::seconds(5);

[[noreturn]] void fail(const char* operation, HRESULT result,
                       ID3D12Device* device = nullptr) {
    std::ostringstream message;
    message << operation << " failed (HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result) << ')';
    if (device) {
        const auto removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            message << "; device removed reason 0x"
                    << static_cast<unsigned long>(removed);
        }
    }
    throw std::runtime_error(message.str());
}

void check(HRESULT result, const char* operation,
           ID3D12Device* device = nullptr) {
    if (FAILED(result)) {
        fail(operation, result, device);
    }
}

std::string utf8(const wchar_t* text) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                        -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("Unable to convert DXGI adapter name to UTF-8");
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                            result.data(), count, nullptr, nullptr) != count) {
        throw std::runtime_error("Unable to convert DXGI adapter name to UTF-8");
    }
    result.pop_back();
    return result;
}
} // namespace

struct Renderer::Impl {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    std::array<ComPtr<ID3D12Resource>, buffer_count> buffers;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    UINT rtv_stride = 0;
    UINT width = 0;
    UINT height = 0;
    UINT swapchain_flags = 0;
    UINT64 next_fence = 1;
    UINT64 submitted_fence = 0;
    bool ready = false;
    bool suspended = false;
    std::string name;

    void wait(UINT64 value) {
        if (value == 0) {
            return;
        }
        // Polling avoids retaining an OS event registration after a GPU timeout.
        // This conservative foundation uses one allocator. Parallel frame
        // resources and latency tuning are deferred until profiled game workloads.
        const auto deadline = std::chrono::steady_clock::now() + gpu_timeout;
        for (;;) {
            const auto completed = fence->GetCompletedValue();
            if (completed == std::numeric_limits<UINT64>::max()) {
                fail("GPU fence reported device removal", DXGI_ERROR_DEVICE_REMOVED,
                     device.Get());
            }
            if (completed >= value) {
                return;
            }
            check(device->GetDeviceRemovedReason(), "GPU device status", device.Get());
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(
                    "GPU fence wait exceeded 5000 ms; runtime must terminate safely");
            }
            Sleep(1);
        }
    }

    void flush() {
        if (!queue || !fence) {
            return;
        }
        const UINT64 value = next_fence++;
        check(queue->Signal(fence.Get(), value), "Signal GPU fence", device.Get());
        submitted_fence = value;
        wait(value);
    }

    void create_views() {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < buffer_count; ++i) {
            check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])),
                  "Get swap-chain buffer", device.Get());
            device->CreateRenderTargetView(buffers[i].Get(), nullptr, handle);
            handle.ptr += rtv_stride;
        }
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

void Renderer::initialize(HWND hwnd, std::uint32_t width, std::uint32_t height,
                          bool warp, bool validation) {
    if (impl_) {
        throw std::logic_error("Renderer is already initialized");
    }
    if (!IsWindow(hwnd) || width == 0 || height == 0 || width > 16384 || height > 16384) {
        throw std::invalid_argument("Renderer needs a valid HWND and dimensions in 1..16384");
    }
    // Keep partial construction owned so shutdown handles initialization failures.
    impl_ = std::make_unique<Impl>();
    auto& state = *impl_;
    UINT factory_flags = 0;
    if (validation) {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),
              "Enable D3D12 validation (install Windows Graphics Tools if unavailable)");
        debug->EnableDebugLayer();
        factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    check(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&state.factory)),
          "Create DXGI factory");

    ComPtr<IDXGIAdapter1> selected;
    if (warp) {
        check(state.factory->EnumWarpAdapter(IID_PPV_ARGS(&selected)),
              "Select explicitly requested WARP software adapter");
    } else {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = state.factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            check(result, "Enumerate graphics adapters");
            DXGI_ADAPTER_DESC1 description{};
            check(candidate->GetDesc1(&description), "Read graphics adapter description");
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                           __uuidof(ID3D12Device), nullptr))) {
                selected = std::move(candidate);
                break;
            }
        }
        if (!selected) {
            throw std::runtime_error(
                "No DirectX 12 hardware adapter is available. Use --warp only for "
                "explicit software-renderer diagnostics; it is not a gameplay target.");
        }
    }

    DXGI_ADAPTER_DESC1 description{};
    check(selected->GetDesc1(&description), "Read selected graphics adapter");
    state.name = utf8(description.Description);
    if (warp) {
        state.name += " (WARP software diagnostics)";
    }
    check(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_11_0,
                           IID_PPV_ARGS(&state.device)), "Create DirectX 12 device");

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(state.device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&state.queue)),
          "Create graphics command queue", state.device.Get());

    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing_supported = FALSE;
    if (SUCCEEDED(state.factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                               &tearing_supported, sizeof(tearing_supported))) &&
        tearing_supported) {
        state.swapchain_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_DESC1 swap_description{};
    swap_description.Width = width;
    swap_description.Height = height;
    swap_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_description.SampleDesc.Count = 1;
    swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_description.BufferCount = buffer_count;
    swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_description.Scaling = DXGI_SCALING_STRETCH;
    swap_description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swap_description.Flags = state.swapchain_flags;
    ComPtr<IDXGISwapChain1> swapchain;
    check(state.factory->CreateSwapChainForHwnd(state.queue.Get(), hwnd,
          &swap_description, nullptr, nullptr, &swapchain),
          "Create window swap chain", state.device.Get());
    check(swapchain.As(&state.swapchain), "Query swap-chain interface");
    check(state.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
          "Disable unmanaged exclusive-fullscreen transition");

    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = buffer_count;
    check(state.device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&state.rtv_heap)),
          "Create render-target descriptor heap", state.device.Get());
    state.rtv_stride = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    state.create_views();
    check(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&state.allocator)), "Create graphics allocator", state.device.Get());
    check(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          state.allocator.Get(), nullptr, IID_PPV_ARGS(&state.commands)),
          "Create graphics command list", state.device.Get());
    check(state.commands->Close(), "Close initial graphics command list", state.device.Get());
    check(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)),
          "Create GPU synchronization fence", state.device.Get());
    state.width = width;
    state.height = height;
    state.ready = true;
}

void Renderer::resize(std::uint32_t width, std::uint32_t height) {
    if (!impl_ || !impl_->ready) {
        throw std::logic_error("Cannot resize an uninitialized renderer");
    }
    auto& state = *impl_;
    if (width > 16384 || height > 16384) {
        throw std::invalid_argument("Render dimensions exceed the supported 16384 limit");
    }
    state.suspended = (width == 0 || height == 0);
    if (state.suspended || (width == state.width && height == state.height)) {
        return;
    }
    state.flush();
    for (auto& buffer : state.buffers) {
        buffer.Reset();
    }
    // The closed command list may retain back-buffer references. Release it
    // before ResizeBuffers and recreate it against the existing allocator.
    state.commands.Reset();
    check(state.swapchain->ResizeBuffers(buffer_count, width, height,
          DXGI_FORMAT_R8G8B8A8_UNORM, state.swapchain_flags),
          "Resize swap-chain buffers", state.device.Get());
    state.create_views();
    check(state.allocator->Reset(), "Reset graphics allocator after resize", state.device.Get());
    check(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          state.allocator.Get(), nullptr, IID_PPV_ARGS(&state.commands)),
          "Recreate graphics command list after resize", state.device.Get());
    check(state.commands->Close(), "Close resized graphics command list", state.device.Get());
    state.width = width;
    state.height = height;
}

void Renderer::render(bool vsync) {
    if (!impl_ || !impl_->ready) {
        throw std::logic_error("Cannot render before initialization");
    }
    auto& state = *impl_;
    if (state.suspended) {
        return;
    }
    state.wait(state.submitted_fence);
    check(state.allocator->Reset(), "Reset graphics allocator", state.device.Get());
    check(state.commands->Reset(state.allocator.Get(), nullptr),
          "Reset graphics command list", state.device.Get());

    const auto index = state.swapchain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = state.buffers[index].Get();
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    state.commands->ResourceBarrier(1, &transition);
    auto target = state.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    target.ptr += static_cast<SIZE_T>(index) * state.rtv_stride;
    constexpr float clear_color[]{0.035F, 0.055F, 0.085F, 1.0F};
    state.commands->ClearRenderTargetView(target, clear_color, 0, nullptr);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    state.commands->ResourceBarrier(1, &transition);
    check(state.commands->Close(), "Close recorded graphics command list", state.device.Get());
    ID3D12CommandList* lists[]{state.commands.Get()};
    state.queue->ExecuteCommandLists(1, lists);
    const UINT64 submitted = state.next_fence++;
    check(state.queue->Signal(state.fence.Get(), submitted),
          "Signal submitted frame", state.device.Get());
    state.submitted_fence = submitted;
    const UINT flags = (!vsync && (state.swapchain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
                           ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const auto result = state.swapchain->Present(vsync ? 1 : 0, flags);
    check(result, "Present frame", state.device.Get());
    if (result == DXGI_STATUS_OCCLUDED) {
        Sleep(16);
    }
}

void Renderer::wait_idle() {
    if (impl_) {
        impl_->flush();
    }
}

void Renderer::shutdown() noexcept {
    if (!impl_) {
        return;
    }
    try {
        impl_->flush();
    } catch (const std::exception& error) {
        // The caller already receives operational failures from render/resize.
        // Destruction cannot throw, but teardown failures remain visible to a
        // debugger instead of being silently hidden.
        OutputDebugStringA("Renderer shutdown could not complete a GPU fence: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    }
    impl_.reset();
}

std::string Renderer::adapter_name() const {
    return impl_ ? impl_->name : std::string{};
}

} // namespace reborn
