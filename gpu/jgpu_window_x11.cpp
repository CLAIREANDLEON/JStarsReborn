// JStarsReborn: X11 game window for the Linux development harness (Xvfb or a real display).
// GPL-2.0-or-later.
#include <X11/Xlib.h>
#include <atomic>
#include <thread>
#include "sdl_window.h"
#include "jgpu.h"
#include "common/logging/log.h"
extern "C" void (*jgpu_log_sink)(const char*);
extern JgpuHost* jgpu_host_ptr();

namespace Frontend {
struct WindowImpl {
    Display* dpy{};
    ::Window win{};
    std::thread pump;
    std::atomic<bool> stop{false};
};

WindowSDL::WindowSDL(s32 w, s32 h, const char* title) : width(w), height(h) {
    XInitThreads();
    impl = new WindowImpl;
    impl->dpy = XOpenDisplay(nullptr);
    if (!impl->dpy) {
        LOG_CRITICAL(Frontend, "XOpenDisplay failed (no DISPLAY); start Xvfb or run on a display");
        is_open = false;
        return;
    }
    const int screen = DefaultScreen(impl->dpy);
    impl->win = XCreateSimpleWindow(impl->dpy, RootWindow(impl->dpy, screen), 0, 0, w, h, 0, 0, BlackPixel(impl->dpy, screen));
    XStoreName(impl->dpy, impl->win, title);
    Atom del = XInternAtom(impl->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(impl->dpy, impl->win, &del, 1);
    XSelectInput(impl->dpy, impl->win, StructureNotifyMask | ExposureMask);
    XMapWindow(impl->dpy, impl->win);
    XFlush(impl->dpy);
    window_info.type = WindowSystemType::X11;
    window_info.display_connection = impl->dpy;
    window_info.render_surface = reinterpret_cast<void*>(impl->win);
    WindowImpl* im = impl;
    impl->pump = std::thread([this, im, del] {
        // Events are read on a second connection-free poll: XPending on the shared Display is serialized
        // by XInitThreads; Vulkan's WSI uses its own xcb connection for the swapchain.
        while (!im->stop) {
            while (XPending(im->dpy)) {
                XEvent e;
                XNextEvent(im->dpy, &e);
                if (e.type == ConfigureNotify) { width = e.xconfigure.width; height = e.xconfigure.height; }
                else if (e.type == ClientMessage && static_cast<Atom>(e.xclient.data.l[0]) == del) {
                    is_open = false;
                    if (auto* h = jgpu_host_ptr(); h && h->window_closed) h->window_closed();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    });
}
WindowSDL::~WindowSDL() {
    if (!impl) return;
    impl->stop = true;
    if (impl->pump.joinable()) impl->pump.join();
    if (impl->dpy) { XDestroyWindow(impl->dpy, impl->win); XCloseDisplay(impl->dpy); }
    delete impl;
}
} // namespace Frontend

// No keyboard controller source on X11 yet (see docs/INPUT.md).
extern "C" int jgpu_display_vblank(int64_t* vblank_qpc, int64_t* period_qpc, int64_t* now_qpc, int64_t* qpc_hz) {
    (void)vblank_qpc; (void)period_qpc; (void)now_qpc; (void)qpc_hz;
    return 0; // no refresh lock on the Linux harness
}
extern "C" int jgpu_window_focused(void) { return 0; }
