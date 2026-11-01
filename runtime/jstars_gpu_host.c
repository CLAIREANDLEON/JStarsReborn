// Host side of the GPU library (gpu/): callbacks the C++ library needs from the runtime, the fault
// hand-off for page tracking, and the memory notifications. With JSTARS_GPU undefined the hooks are
// no-ops and the runtime behaves like checkpoint 0.2.2 (stops at the first Gnm call).
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef JSTARS_GPU
#include "jgpu.h"
static int g_gpu_on;
int rt_gpu_enabled(void) { return g_gpu_on; }
void* rt_gpu_resolve(const char* nid_lib) { return g_gpu_on ? jgpu_resolve(nid_lib) : NULL; }
const char* rt_gpu_symbol_name(const char* nid_lib) { return g_gpu_on ? jgpu_symbol_name(nid_lib) : ""; }
void rt_gpu_mem_map(void* addr, uint64_t len) { if (g_gpu_on) jgpu_mem_map((uintptr_t)addr, len); }
void rt_gpu_mem_unmap(void* addr, uint64_t len) { if (g_gpu_on) jgpu_mem_unmap((uintptr_t)addr, len); }

static void host_log(const char* line) { rt_logf("%s", line); }
static void host_fatal(const char* what) {
    rt_logf("GPU FATAL: %s", what);
    fflush(NULL);
    _exit(5);
}
static int host_protect(void* a, size_t n, int prot) { return os_protect(a, n, prot); }
static size_t host_safe_read(void* d, const void* s, size_t n) { return os_safe_read(d, s, n); }
static void host_window_closed(void) {
    rt_logf("game window closed by the player");
    fflush(NULL);
    _exit(0);
}
static int host_is_neo(void) { return 0; } // the runtime presents itself as a base PS4 (no Pro-only paths)
static int fault_prehook(const OsFault* f) {
    if (!g_gpu_on) return 0;
    return jgpu_handle_fault(f->native, f->address);
}

int rt_gpu_start(const char* title, const char* data_dir, const char* capture_dir, int width, int height) {
    JgpuHost host = {host_log, host_fatal, host_protect, host_safe_read, rt_fd_alloc, rt_fd_free, host_window_closed,
                   rt_errno_cell, rt_read_tsc, rt_tsc_frequency, rt_process_time_us, rt_compiled_sdk_version, host_is_neo};
    jgpu_set_host(&host);
    JgpuConfig cfg = {title, data_dir, capture_dir, width, height, 0};
    rt_logf("GPU library: starting (window %dx%d, data %s, captures %s)", width, height, data_dir, capture_dir);
    if (jgpu_init(&cfg)) { rt_logf("GPU library: initialization failed"); return 1; }
    g_gpu_on = 1;
    rt_input_set_focus_hook(jgpu_window_focused);
    os_set_fault_prehook(fault_prehook);
    rt_logf("GPU library: ready, %u exports registered", jgpu_symbol_count());
    return 0;
}
void rt_gpu_capture(int n) { if (g_gpu_on) jgpu_request_capture(n); }
void rt_gpu_stats(uint64_t out[5]) {
    JgpuStats s = {0};
    if (g_gpu_on) jgpu_get_stats(&s);
    out[0] = s.draws; out[1] = s.dispatches; out[2] = s.flips_requested; out[3] = s.gnm_frames; out[4] = s.mem_maps;
}
#else
int rt_gpu_enabled(void) { return 0; }
void* rt_gpu_resolve(const char* nid_lib) { (void)nid_lib; return NULL; }
const char* rt_gpu_symbol_name(const char* nid_lib) { (void)nid_lib; return ""; }
void rt_gpu_mem_map(void* addr, uint64_t len) { (void)addr; (void)len; }
void rt_gpu_mem_unmap(void* addr, uint64_t len) { (void)addr; (void)len; }
int rt_gpu_start(const char* t, const char* d, const char* c, int w, int h) { (void)t; (void)d; (void)c; (void)w; (void)h; rt_logf("this build has no GPU library"); return 1; }
void rt_gpu_capture(int n) { (void)n; }
void rt_gpu_stats(uint64_t out[5]) { memset(out, 0, 5 * sizeof *out); }
#endif
