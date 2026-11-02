// Host side of the GPU library (gpu/): callbacks the C++ library needs from the runtime, the fault
// hand-off for page tracking, and the memory notifications. With JSTARS_GPU undefined the hooks are
// no-ops and the runtime behaves like checkpoint 0.2.2 (stops at the first Gnm call).
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef JSTARS_GPU
#include "jgpu.h"
static int g_gpu_on;
int rt_gpu_enabled(void) { return g_gpu_on; }
// ---- frame timing telemetry and controlled flip-rate experiments (docs/GRAPHICS.md) ----------------------------------
// The guest submits each finished frame with sceGnmSubmitAndFlipCommandBuffers and chooses how many vblanks a frame stays
// on screen with sceVideoOutSetFlipRate (0 = every vblank, 1 = every second vblank, ...). Wrapping those two imports gives
// the guest's real rendered frame rate and frame-time distribution, separately from what the presenter shows, and lets an
// experiment override the requested flip rate. Nothing here changes behaviour unless --flip-rate is given.
typedef int64_t (GUEST_ABI* GuestFn10)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static GuestFn10 g_real_submit_flip, g_real_set_flip_rate;
static int g_frame_stats, g_flip_rate_override = -1;
static int g_requested_flip_rate = -1;
#define FT_RING 4096
static uint64_t g_ft_tsc[FT_RING];
static uint64_t g_ft_count;
static pthread_mutex_t g_ft_lock = PTHREAD_MUTEX_INITIALIZER;

void rt_gpu_timing_configure(int frame_stats, int flip_rate_override) { g_frame_stats = frame_stats; g_flip_rate_override = flip_rate_override; }

static int64_t GUEST_ABI submit_flip_wrapper(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    const uint64_t now = rt_read_tsc();
    pthread_mutex_lock(&g_ft_lock);
    g_ft_tsc[g_ft_count % FT_RING] = now;
    ++g_ft_count;
    pthread_mutex_unlock(&g_ft_lock);
    return g_real_submit_flip(a, b, c, d, e, f, g, h, i, j);
}
static int64_t GUEST_ABI set_flip_rate_wrapper(uint64_t handle, uint64_t rate, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    if ((int)rate != g_requested_flip_rate) {
        g_requested_flip_rate = (int)rate;
        rt_logf("frame timing: the game asks for flip rate %d (a frame every %d vblank%s)%s", (int)rate, (int)rate + 1, rate ? "s" : "",
                g_flip_rate_override >= 0 ? "; overridden by --flip-rate (experiment)" : "");
    }
    if (g_flip_rate_override >= 0) rate = (uint64_t)g_flip_rate_override;
    return g_real_set_flip_rate(handle, rate, c, d, e, f, g, h, i, j);
}
static int cmp_u64(const void* x, const void* y) { const uint64_t a = *(const uint64_t*)x, b = *(const uint64_t*)y; return a < b ? -1 : a > b; }

// Called once a second by the monitor: every 5 s, the guest frames submitted in that window and their frame times.
void rt_gpu_timing_tick(int sec) {
    if (!g_frame_stats || sec % 5) return;
    static uint64_t last_count;
    static uint64_t iv[FT_RING];
    pthread_mutex_lock(&g_ft_lock);
    const uint64_t count = g_ft_count;
    uint64_t n = count - last_count;
    if (n > FT_RING - 1) n = FT_RING - 1;
    int k = 0;
    uint64_t start = count - n;
    if (start == 0) start = 1;
    for (uint64_t x = start; x < count && k < FT_RING; ++x) iv[k++] = g_ft_tsc[x % FT_RING] - g_ft_tsc[(x - 1) % FT_RING];
    pthread_mutex_unlock(&g_ft_lock);
    const uint64_t frames = count - last_count;
    last_count = count;
    if (!k) { rt_logf("frame timing t=%ds: %llu guest frames in the last 5 s", sec, (unsigned long long)frames); return; }
    qsort(iv, (size_t)k, sizeof iv[0], cmp_u64);
    const double ms = 1000.0 / (double)rt_tsc_frequency();
    rt_logf("frame timing t=%ds: %llu guest frames in 5 s = %.2f fps; frame time ms p50 %.2f p95 %.2f p99 %.2f max %.2f (game flip rate %d%s)",
            sec, (unsigned long long)frames, frames / 5.0, iv[k / 2] * ms, iv[(k * 95) / 100] * ms, iv[(k * 99) / 100] * ms, iv[k - 1] * ms,
            g_requested_flip_rate, g_flip_rate_override >= 0 ? ", overridden" : "");
}

void* rt_gpu_resolve(const char* nid_lib) {
    if (!g_gpu_on) return NULL;
    void* fn = jgpu_resolve(nid_lib);
    if (!fn) return NULL;
    if (!strcmp(nid_lib, "xbxNatawohc#libSceGnmDriver")) { g_real_submit_flip = (GuestFn10)fn; return (void*)submit_flip_wrapper; }   // sceGnmSubmitAndFlipCommandBuffers
    if (!strcmp(nid_lib, "CBiu4mCE1DA#libSceVideoOut")) { g_real_set_flip_rate = (GuestFn10)fn; return (void*)set_flip_rate_wrapper; }  // sceVideoOutSetFlipRate
    return fn;
}
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
void rt_gpu_fsr_probe(void) { if (g_gpu_on) { jgpu_fsr_probe(); rt_logf("%s", jgpu_fsr_report()); } }
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
void rt_gpu_timing_configure(int frame_stats, int flip_rate_override) { (void)frame_stats; (void)flip_rate_override; }
void rt_gpu_timing_tick(int sec) { (void)sec; }
void rt_gpu_fsr_probe(void) {}
#endif
