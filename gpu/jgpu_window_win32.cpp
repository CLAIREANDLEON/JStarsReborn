// JStarsReborn: native Win32 game window. The window lives on its own thread with a message loop;
// the renderer only reads its size and HWND. GPL-2.0-or-later.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "sdl_window.h"
#include "jgpu.h"
#include "common/logging/log.h"
extern JgpuHost* jgpu_host_ptr();

static std::atomic<HWND> g_main_hwnd{nullptr};
bool jgpu_window_start_fullscreen = false; // set by jgpu_init from JgpuConfig.fullscreen

// Borderless fullscreen on the window's monitor, and back to the previous window (Alt+Enter or F11). Runs on the window thread.
static WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};
static bool g_is_fullscreen = false;
static void set_fullscreen(HWND hwnd, bool on) {
    if (on == g_is_fullscreen) return;
    if (on) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        GetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    g_is_fullscreen = on;
    LOG_INFO(Frontend, "window: {}", on ? "borderless fullscreen" : "windowed");
}
extern "C" int jgpu_window_focused(void) {
    HWND h = g_main_hwnd.load();
    return h && GetForegroundWindow() == h;
}

namespace Frontend {
struct WindowImpl {
    HWND hwnd{};
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    bool ready{};
    WindowSDL* owner{};
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WindowSDL*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (self && wp != SIZE_MINIMIZED) self->OnResize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_SYSKEYDOWN: // Alt+Enter (bit 29: Alt held); no system beep
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            if (!(lp & (1 << 30))) set_fullscreen(hwnd, !g_is_fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) { set_fullscreen(hwnd, !g_is_fullscreen); return 0; }
        break;
    case WM_CLOSE:
        if (self) self->OnClose();
        if (auto* h = jgpu_host_ptr(); h && h->window_closed) h->window_closed();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

WindowSDL::WindowSDL(s32 w, s32 h, const char* title) : width(w), height(h) {
    impl = new WindowImpl;
    impl->owner = this;
    std::string t = title;
    impl->thread = std::thread([this, t, w, h] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = L"JStarsRebornGameWindow";
        RegisterClassW(&wc);
        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        std::wstring wt(t.begin(), t.end());
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, wt.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                    r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            if (jgpu_window_start_fullscreen) set_fullscreen(hwnd, true);
            RECT cr;
            GetClientRect(hwnd, &cr);
            width = cr.right - cr.left;
            height = cr.bottom - cr.top;
        }
        {
            std::scoped_lock lk{impl->m};
            impl->hwnd = hwnd;
            g_main_hwnd.store(hwnd);
            impl->ready = true;
        }
        impl->cv.notify_all();
        MSG msg;
        while (hwnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });
    std::unique_lock lk{impl->m};
    impl->cv.wait(lk, [this] { return impl->ready; });
    if (!impl->hwnd) {
        LOG_CRITICAL(Frontend, "CreateWindowEx failed: {}", GetLastError());
        is_open = false;
        return;
    }
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = impl->hwnd;
    window_info.display_connection = nullptr;
}
WindowSDL::~WindowSDL() {
    if (!impl) return;
    if (impl->hwnd) PostMessageW(impl->hwnd, WM_CLOSE + 0x1000, 0, 0), DestroyWindow(impl->hwnd);
    if (impl->thread.joinable()) impl->thread.join();
    delete impl;
}
} // namespace Frontend

// ---- compatibility: CreateFileMapping2 -----------------------------------------------------------------------------
// The mingw-w64 import libraries available to the cross build do not list CreateFileMapping2 (Windows 10, kernelbase).
// The vendored IOFile::Map references it through the import table, so provide that table slot and resolve the real
// function at run time. Where it does not exist the call fails with ERROR_CALL_NOT_IMPLEMENTED instead of linking wrongly.
extern "C" {
static HANDLE WINAPI jgpu_CreateFileMapping2(HANDLE file, SECURITY_ATTRIBUTES* sa, ULONG access, ULONG prot, ULONG alloc,
                                              ULONG64 max_size, PCWSTR name, void* ext, ULONG ext_count) {
    using Fn = HANDLE(WINAPI*)(HANDLE, SECURITY_ATTRIBUTES*, ULONG, ULONG, ULONG, ULONG64, PCWSTR, void*, ULONG);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileMapping2"));
    if (!fn) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return nullptr;
    }
    return fn(file, sa, access, prot, alloc, max_size, name, ext, ext_count);
}
void* __imp_CreateFileMapping2 = reinterpret_cast<void*>(&jgpu_CreateFileMapping2);
}
