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
// --flip-rate-after s: the override is applied only from second s on (the game's own rate before that), so a run can reach
// a known scene at the game's rate and then measure game speed against the wall clock with the override in force.
static int g_flip_rate_after;
static int g_flip_override_active;
static int g_flip_override_due, g_flip_override_attempted;
static uint64_t g_flip_handle;
static int g_flip_handle_known;
#define FT_RING 4096
static uint64_t g_ft_tsc[FT_RING];
static uint64_t g_ft_count;
static pthread_mutex_t g_ft_lock = PTHREAD_MUTEX_INITIALIZER;

void rt_gpu_timing_configure(int frame_stats, int flip_rate_override, int flip_rate_after) {
    g_frame_stats = frame_stats;
    g_flip_rate_override = flip_rate_override;
    g_flip_rate_after = flip_rate_after > 0 ? flip_rate_after : 0;
    g_flip_override_active = flip_rate_override >= 0 && g_flip_rate_after == 0;
    g_flip_override_due = g_flip_override_attempted = 0;
    g_flip_handle_known = 0;
}

static int64_t GUEST_ABI submit_flip_wrapper(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    const uint64_t now = rt_read_tsc();
    pthread_mutex_lock(&g_ft_lock);
    // Apply on the guest submission thread, not the host monitor thread. The
    // timing lock serializes this with the game's own SetFlipRate calls.
    if (g_flip_override_due && !g_flip_override_attempted && g_flip_handle_known && g_real_set_flip_rate) {
        g_flip_override_attempted = 1;
        const int64_t rc = g_real_set_flip_rate(g_flip_handle, (uint64_t)g_flip_rate_override, 0, 0, 0, 0, 0, 0, 0, 0);
        g_flip_override_active = rc == 0;
        rt_logf("frame timing: delayed --flip-rate %d attempt on guest submission (threshold %d s; game requested %d; result %lld; %s)",
                g_flip_rate_override, g_flip_rate_after, g_requested_flip_rate, (long long)rc,
                g_flip_override_active ? "override active" : "override failed; game rate retained");
    }
    g_ft_tsc[g_ft_count % FT_RING] = now;
    ++g_ft_count;
    pthread_mutex_unlock(&g_ft_lock);
    return g_real_submit_flip(a, b, c, d, e, f, g, h, i, j);
}
static int64_t GUEST_ABI set_flip_rate_wrapper(uint64_t handle, uint64_t rate, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    pthread_mutex_lock(&g_ft_lock);
    if ((int)rate != g_requested_flip_rate) {
        g_requested_flip_rate = (int)rate;
        rt_logf("frame timing: the game asks for flip rate %d (a frame every %d vblank%s)%s", (int)rate, (int)rate + 1, rate ? "s" : "",
                g_flip_override_active ? "; overridden by --flip-rate (experiment)" : g_flip_rate_override >= 0 ? "; --flip-rate is pending (--flip-rate-after)" : "");
    }
    if (g_flip_override_active) rate = (uint64_t)g_flip_rate_override;
    const int64_t rc = g_real_set_flip_rate(handle, rate, c, d, e, f, g, h, i, j);
    if (rc == 0) { g_flip_handle = handle; g_flip_handle_known = 1; }
    pthread_mutex_unlock(&g_ft_lock);
    return rc;
}
static int cmp_u64(const void* x, const void* y) { const uint64_t a = *(const uint64_t*)x, b = *(const uint64_t*)y; return a < b ? -1 : a > b; }

// Called once a second by the monitor: every 5 s, the guest frames submitted in that window and their frame times.
void rt_gpu_timing_tick(int sec) {
    pthread_mutex_lock(&g_ft_lock);
    if (g_flip_rate_override >= 0 && g_flip_rate_after > 0 && sec >= g_flip_rate_after) g_flip_override_due = 1;
    pthread_mutex_unlock(&g_ft_lock);
    if (!g_frame_stats || sec % 5) return;
    static uint64_t last_count;
    static uint64_t iv[FT_RING];
    pthread_mutex_lock(&g_ft_lock);
    const uint64_t count = g_ft_count;
    const int requested_rate = g_requested_flip_rate;
    const int override_active = g_flip_override_active;
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
            requested_rate, override_active ? ", overridden" : "");
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

// The callbacks the GPU library's shims use (logging, kernel time, SDK version...). The online libraries (gpu/jsnet) use the
// same shims, so they are installed by whichever starts first.
static void install_gpu_host(void) {
    static const JgpuHost host = {host_log, host_fatal, host_protect, host_safe_read, rt_fd_alloc, rt_fd_free, host_window_closed,
                                  rt_errno_cell, rt_read_tsc, rt_tsc_frequency, rt_process_time_us, rt_compiled_sdk_version, host_is_neo};
    jgpu_set_host(&host);
}
int rt_gpu_start(const char* title, const char* data_dir, const char* capture_dir, int width, int height, int fullscreen) {
    install_gpu_host();
    JgpuConfig cfg = {title, data_dir, capture_dir, width, height, 0, fullscreen};
    rt_logf("GPU library: starting (window %dx%d%s, data %s, captures %s)", width, height, fullscreen ? ", borderless fullscreen" : "", data_dir, capture_dir);
    if (jgpu_init(&cfg)) { rt_logf("GPU library: initialization failed"); return 1; }
    g_gpu_on = 1;
    rt_input_set_focus_hook(jgpu_window_focused);
    os_set_fault_prehook(fault_prehook);
    rt_logf("GPU library: ready, %u exports registered", jgpu_symbol_count());
    return 0;
}
void rt_gpu_capture(int n) { if (g_gpu_on) jgpu_request_capture(n); }
float* rt_gpu_widescreen(int enable) { return g_gpu_on ? jgpu_widescreen(enable) : NULL; }
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
int rt_gpu_start(const char* t, const char* d, const char* c, int w, int h, int f) { (void)t; (void)d; (void)c; (void)w; (void)h; (void)f; rt_logf("this build has no GPU library"); return 1; }
void rt_gpu_capture(int n) { (void)n; }
float* rt_gpu_widescreen(int enable) { (void)enable; return NULL; }
void rt_gpu_stats(uint64_t out[5]) { memset(out, 0, 5 * sizeof *out); }
void rt_gpu_timing_configure(int frame_stats, int flip_rate_override, int flip_rate_after) { (void)frame_stats; (void)flip_rate_override; (void)flip_rate_after; }
void rt_gpu_timing_tick(int sec) { (void)sec; }
void rt_gpu_fsr_probe(void) {}
#endif

// ---------------------------------------------------------------- online libraries (gpu/jsnet, --shadnet; docs/ADR-008-online-shadnet.md)
#ifdef JSTARS_JSNET
void jsnet_configure(const char* app0, const char* serial, const char* title, const char* app_ver);
void jsnet_register(void);
void jsnet_initialize(void);
void* jsnet_resolve(const char* nid_lib);
unsigned jsnet_symbol_count(void);
static int g_jsnet_on;
// param.sfo: "\0PSF" header, key table at +8, data table at +12, entry count at +16, 16-byte entries from +0x14.
static int sfo_value(const char* path, const char* key, char* out, size_t cap) {
    out[0] = 0;
    FILE* f = os_fopen(path, "rb");
    if (!f) return -1;
    static unsigned char b[65536];
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 0x14 || memcmp(b, "\0PSF", 4)) return -1;
    const uint32_t kt = b[8] | b[9] << 8 | b[10] << 16 | (uint32_t)b[11] << 24, dt = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
    const uint32_t cnt = b[16] | b[17] << 8 | b[18] << 16 | (uint32_t)b[19] << 24;
    for (uint32_t i = 0; i < cnt && 0x14 + (i + 1) * 16 <= n; ++i) {
        const unsigned char* e = b + 0x14 + i * 16;
        const uint32_t ko = e[0] | e[1] << 8, len = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
        const uint32_t dof = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        if (kt + ko >= n || strcmp((const char*)b + kt + ko, key) || dt + dof + len > n) continue;
        snprintf(out, cap, "%.*s", (int)len, (const char*)b + dt + dof);
        return 0;
    }
    return -1;
}
int rt_jsnet_available(void) { return 1; }
void rt_jsnet_setup(const char* game_root) {
    char sfo[4096], serial[16], title[128], ver[16];
    snprintf(sfo, sizeof sfo, "%s/sce_sys/param.sfo", game_root ? game_root : ".");
    sfo_value(sfo, "TITLE_ID", serial, sizeof serial);
    sfo_value(sfo, "TITLE", title, sizeof title);
    sfo_value(sfo, "APP_VER", ver, sizeof ver);
    if (!g_gpu_on) install_gpu_host();
    jsnet_configure(game_root ? game_root : ".", serial, title, ver);
    jsnet_register();
    g_jsnet_on = 1;
    rt_logf("shadNet: %u network/NP functions from shadPS4 v0.20.0 (gpu/jsnet) replace the runtime's own; game %s %s v%s",
            jsnet_symbol_count(), serial[0] ? serial : "?", title, ver);
}
void* rt_jsnet_resolve(const char* nid_lib) { return g_jsnet_on ? jsnet_resolve(nid_lib) : NULL; }
void rt_jsnet_start(void) { if (g_jsnet_on) { rt_logf("shadNet: starting the online session"); jsnet_initialize(); } }
#else
int rt_jsnet_available(void) { return 0; }
void rt_jsnet_setup(const char* game_root) { (void)game_root; rt_logf("--shadnet: this build has no online libraries (gpu/jsnet)"); }
void* rt_jsnet_resolve(const char* nid_lib) { (void)nid_lib; return NULL; }
void rt_jsnet_start(void) {}
#endif
