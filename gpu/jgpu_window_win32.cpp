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
bool jgpu_window_match_refresh = false;    // set by jgpu_init from JgpuConfig.match_refresh

// ---- refresh matching ------------------------------------------------------------------------------------------------
// The game presents at 30 or 60 fps. On a monitor whose refresh rate is not a multiple of 60 (144, 165, 175 Hz...) and without
// variable refresh, each frame then stays on screen for an uneven number of refreshes (60 fps at 175 Hz: 2 or 3 refreshes, 11.4 or
// 17.1 ms), which looks like stutter although every frame arrives on time. In fullscreen, refresh matching switches the monitor
// temporarily (CDS_FULLSCREEN: never written to the registry, undone by Windows when the process ends) to the same resolution at a
// multiple of 60 Hz, preferring 120 Hz, and restores it when fullscreen ends.
static wchar_t g_changed_device[CCHDEVICENAME];
static bool g_refresh_changed = false;
static bool cadence_ok(DWORD hz) { return hz % 60 <= 1 || (hz + 1) % 60 == 0; } // 59/60, 119/120, 239/240, 359/360, 479/480...
static int refresh_rank(DWORD hz) { // higher is better; 0 = not a multiple of 60
    switch (hz) { case 120: return 6; case 119: return 5; case 240: return 4; case 239: return 3; case 60: return 2; case 59: return 1; }
    return 0;
}
static void log_display(HMONITOR mon) {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    DEVMODEW cur{}; cur.dmSize = sizeof(cur);
    if (!GetMonitorInfoW(mon, &mi) || !EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur, 0)) return;
    LOG_INFO(Frontend, "display: {}x{} at {} Hz{}", cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency,
             cadence_ok(cur.dmDisplayFrequency) ? "" : " (not a multiple of 60: 30/60 fps frames are shown for uneven times without variable refresh)");
}
static void match_refresh(HMONITOR mon) {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    DEVMODEW cur{}; cur.dmSize = sizeof(cur);
    if (!GetMonitorInfoW(mon, &mi) || !EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur, 0)) return;
    if (cadence_ok(cur.dmDisplayFrequency)) {
        LOG_INFO(Frontend, "refresh matching: the display already runs at {} Hz; unchanged", cur.dmDisplayFrequency);
        return;
    }
    DEVMODEW best{}; int best_rank = 0;
    DEVMODEW m{}; m.dmSize = sizeof(m);
    for (DWORD i = 0; EnumDisplaySettingsExW(mi.szDevice, i, &m, 0); ++i) {
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight || m.dmBitsPerPel != cur.dmBitsPerPel) continue;
        const int rank = refresh_rank(m.dmDisplayFrequency);
        if (rank > best_rank) { best = m; best_rank = rank; }
    }
    if (!best_rank) {
        LOG_WARNING(Frontend, "refresh matching: {}x{} offers no 60/120/240 Hz mode; staying at {} Hz", cur.dmPelsWidth, cur.dmPelsHeight,
                    cur.dmDisplayFrequency);
        return;
    }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    const LONG r = ChangeDisplaySettingsExW(mi.szDevice, &best, nullptr, CDS_FULLSCREEN, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) {
        wcsncpy(g_changed_device, mi.szDevice, CCHDEVICENAME - 1);
        g_refresh_changed = true;
        LOG_INFO(Frontend, "refresh matching: display switched from {} Hz to {} Hz while fullscreen", cur.dmDisplayFrequency,
                 best.dmDisplayFrequency);
    } else {
        LOG_WARNING(Frontend, "refresh matching: switching to {} Hz failed ({}); staying at {} Hz", best.dmDisplayFrequency, r,
                    cur.dmDisplayFrequency);
    }
}
static void restore_refresh() {
    if (!g_refresh_changed) return;
    ChangeDisplaySettingsExW(g_changed_device, nullptr, nullptr, 0, nullptr); // back to the registry (desktop) mode
    g_refresh_changed = false;
    LOG_INFO(Frontend, "refresh matching: desktop refresh rate restored");
}

// Borderless fullscreen on the window's monitor, and back to the previous window (Alt+Enter or F11). Runs on the window thread.
static WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};
static bool g_is_fullscreen = false;
static void set_fullscreen(HWND hwnd, bool on) {
    if (on == g_is_fullscreen) return;
    if (on) {
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (jgpu_window_match_refresh) match_refresh(mon);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) return;
        GetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        restore_refresh();
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    g_is_fullscreen = on;
    LOG_INFO(Frontend, "window: {}", on ? "borderless fullscreen" : "windowed");
}
// ---- display vblank timing (refresh lock in videoout/driver.cpp) ------------------------------------------------------------
// DWM reports the primary display's last vblank and its refresh period in QPC units (the game window is on the primary display
// in fullscreen and normally in a window too). dwmapi is loaded at run time; nothing is linked. Returns 0 when not available.
// The query runs on a helper thread four times a second and the present thread only reads the newest sample:
// DwmGetCompositionTimingInfo goes through the compositor, and the present thread must never wait on it (asked at every
// vblank it stood still for about 170 ms every few seconds, with the game waiting on it). An older vblank serves the lock
// as well as the newest one, since the display grid is extrapolated from it in steps of the period either way.
#include <dwmapi.h>
namespace {
typedef HRESULT(WINAPI* DwmGetTiming)(HWND, DWM_TIMING_INFO*);
DwmGetTiming display_timing_function() {
    static DwmGetTiming get_timing = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmGetTiming>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo"))) : nullptr;
    }();
    return get_timing;
}
// The newest sample, double-buffered: the helper thread fills the buffer the present thread is not reading, then publishes it.
struct DisplayTimingSample {
    std::atomic<int64_t> vblank_qpc{0};
    std::atomic<int64_t> period_qpc{0};
};
DisplayTimingSample g_display_timing[2];
std::atomic<int> g_display_timing_current{-1};      // index of the published sample; -1: none
std::atomic<int64_t> g_display_timing_slowest_ns{0}; // the longest query since the last jgpu_display_vblank_stats
std::atomic<int> g_display_timing_slow{0};          // queries over 2 ms since then
std::atomic<int> g_display_timing_stale{0};         // samples that repeated the previous vblank since then
std::atomic<bool> g_display_timing_started{false};
int64_t g_display_timing_last_vblank = 0; // helper thread only

// One query; publishes a sample when the compositor reports timing. Runs on the helper thread (and once at the start).
void display_timing_query() {
    DwmGetTiming get_timing = display_timing_function();
    LARGE_INTEGER hz{};
    if (!get_timing || !QueryPerformanceFrequency(&hz) || !hz.QuadPart) return;
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    const HRESULT hr = get_timing(nullptr, &info);
    QueryPerformanceCounter(&t1);
    const int64_t took_ns = (t1.QuadPart - t0.QuadPart) * 1000000000 / hz.QuadPart;
    if (took_ns > g_display_timing_slowest_ns.load(std::memory_order_relaxed)) g_display_timing_slowest_ns.store(took_ns, std::memory_order_relaxed);
    if (took_ns > 2000000) g_display_timing_slow.fetch_add(1, std::memory_order_relaxed);
    if (FAILED(hr) || !info.qpcRefreshPeriod) {
        g_display_timing_current.store(-1, std::memory_order_release);
        return;
    }
    const int64_t vblank = static_cast<int64_t>(info.qpcVBlank);
    if (vblank == g_display_timing_last_vblank) g_display_timing_stale.fetch_add(1, std::memory_order_relaxed);
    g_display_timing_last_vblank = vblank;
    const int next = g_display_timing_current.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    g_display_timing[next].vblank_qpc.store(vblank, std::memory_order_relaxed);
    g_display_timing[next].period_qpc.store(static_cast<int64_t>(info.qpcRefreshPeriod), std::memory_order_relaxed);
    g_display_timing_current.store(next, std::memory_order_release);
}
void display_timing_thread() {
    for (;;) {
        Sleep(250);
        display_timing_query();
    }
}
} // namespace

extern "C" int jgpu_display_vblank(int64_t* vblank_qpc, int64_t* period_qpc, int64_t* now_qpc, int64_t* qpc_hz) {
    if (!g_display_timing_started.exchange(true)) {
        display_timing_query(); // the first sample at once, so the lock starts with the first frames
        std::thread(display_timing_thread).detach();
    }
    LARGE_INTEGER now{}, hz{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&hz) || !hz.QuadPart) return 0;
    const int cur = g_display_timing_current.load(std::memory_order_acquire);
    if (cur < 0) return 0;
    const int64_t vblank = g_display_timing[cur].vblank_qpc.load(std::memory_order_relaxed);
    const int64_t period = g_display_timing[cur].period_qpc.load(std::memory_order_relaxed);
    // A vblank over a second old means the compositor stopped reporting (or the helper thread is stuck): no lock then.
    if (period <= 0 || now.QuadPart - vblank > hz.QuadPart) return 0;
    *vblank_qpc = vblank;
    *period_qpc = period;
    *now_qpc = now.QuadPart;
    *qpc_hz = hz.QuadPart;
    return 1;
}

// For the present timing log: the longest display timing query since the last call, how many took over 2 ms, and how many
// samples repeated the previous vblank (a compositor that stopped composing); all reset.
extern "C" void jgpu_display_vblank_stats(int64_t* slowest_ns, int* slow, int* stale) {
    *slowest_ns = g_display_timing_slowest_ns.exchange(0, std::memory_order_relaxed);
    *slow = g_display_timing_slow.exchange(0, std::memory_order_relaxed);
    *stale = g_display_timing_stale.exchange(0, std::memory_order_relaxed);
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
        restore_refresh();
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
            log_display(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
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
