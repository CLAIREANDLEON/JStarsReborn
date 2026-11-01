// JStarsReborn: the game window as the vendored presenter sees it. The class keeps the upstream name
// (shadPS4 uses SDL here); this port creates a native Win32 window on Windows and an X11 window on the
// Linux development harness. Implemented in gpu/jgpu_window_*.cpp.
#pragma once
#include <atomic>
#include <string>
#include "common/types.h"

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    void OnResize(s32 w, s32 h) { width = w; height = h; }
    void OnClose() { is_open = false; }

private:
    friend struct WindowImpl;
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    WindowSystemInfo window_info{};
    struct WindowImpl* impl{};
};

} // namespace Frontend
