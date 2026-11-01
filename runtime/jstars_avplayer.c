// libSceAvPlayer: movie playback. THIS FILE DOES NOT DECODE VIDEO OR AUDIO YET.
// Evidence for the event step below (disassembly of the game's callback at eboot.bin+0x376350, Windows run
// 20261101-045232): the game gives the player an event callback, calls AddSource and SetLooping, then makes no further
// call until the callback is invoked with event 2 (READY); that callback enumerates the streams (StreamCount, GetStreamInfo,
// EnableStream) and itself calls sceAvPlayerStart. So READY must be delivered, asynchronously, for a movie to proceed.
// What it does: validates arguments, records the player state the game builds up (init data, sources), inspects each
// source file named by sceAvPlayerAddSource (size and MP4 'ftyp' brand are read from the user's own file, nothing is
// copied or written) and then reports honestly that no stream exists: StreamCount is 0, IsActive is false, GetVideoDataEx
// returns no frame. A movie therefore ends immediately. Every one of these calls is logged on its first use and AddSource
// every time, so the log shows exactly which movies the game asked for. Audit status: diagnostic-stub.
// SceAvPlayerInitData layout (from the public PS4 SDK headers as mirrored by shadPS4/OpenOrbis; NOT verified against this
// title): memory allocator (5 pointers), file replacement (5), event replacement (2), then debug level, base priority,
// output frame buffer count, auto-start flag, default language. The raw bytes are logged so the layout can be checked.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

enum { AVP_ERR_INVALID_PARAMS = (int)0x806A0001, AVP_ERR_OPERATION_FAILED = (int)0x806A0002 };
#define AVP_MAX 8
typedef void (GUEST_ABI* AvpEventCb)(void* obj, int event, int source_id, void* data);
enum { AVP_EVENT_STOP = 1, AVP_EVENT_READY = 2, AVP_EVENT_PLAY = 3 };
typedef struct {
    int used; uint32_t magic;
    void* event_obj; AvpEventCb event_cb;
    int sources, started, looping;
    char path[512];
    uint64_t file_size; char brand[5];
    uint64_t calls_active, calls_video;
} AvpPlayer;
static AvpPlayer g_avp[AVP_MAX];
#define AVP_HANDLE_BASE 0x41560000u   // opaque, non-pointer handle values: the game must not dereference them

static AvpPlayer* avp_get(uint64_t h) {
    if (h <= AVP_HANDLE_BASE || h > AVP_HANDLE_BASE + AVP_MAX) return NULL;
    AvpPlayer* p = &g_avp[h - AVP_HANDLE_BASE - 1];
    return p->used ? p : NULL;
}

static void hexdump(const char* tag, const void* base, size_t n) {
    uint8_t buf[256]; if (n > sizeof buf) n = sizeof buf;
    size_t got = os_safe_read(buf, base, n);
    char line[160];
    for (size_t o = 0; o < got; o += 32) {
        size_t l = 0;
        for (size_t i = o; i < o + 32 && i < got; i += 8) {
            uint64_t v = 0; memcpy(&v, buf + i, got - i >= 8 ? 8 : got - i);
            l += (size_t)snprintf(line + l, sizeof line - l, " %016" PRIx64, v);
        }
        rt_logf("%s +%03zx:%s", tag, o, line);
    }
    if (got < n) rt_logf("%s: only %zu of %zu bytes readable", tag, got, n);
}

static uint64_t GUEST_ABI avp_init(const void* init_data) {
    if (!init_data) { rt_logf("sceAvPlayerInit(NULL): rejected"); return 0; }
    rt_logf("sceAvPlayerInit(init data %p): NO DECODER - movies will not play. Raw init data follows (layout from public SDK headers, unverified):", init_data);
    hexdump("sceAvPlayerInit data", init_data, 128);
    uint64_t w[16]; size_t got = os_safe_read(w, init_data, sizeof w);
    if (got >= sizeof w) {
        rt_logf("sceAvPlayerInit: allocate=%#" PRIx64 " deallocate=%#" PRIx64 " allocateTexture=%#" PRIx64 " deallocateTexture=%#" PRIx64
                " | file replacement: open=%#" PRIx64 " close=%#" PRIx64 " readOffset=%#" PRIx64 " size=%#" PRIx64
                " | event callback=%#" PRIx64, w[1], w[2], w[3], w[4], w[6], w[7], w[8], w[9], w[11]);
    }
    for (int i = 0; i < AVP_MAX; ++i) if (!g_avp[i].used) {
        memset(&g_avp[i], 0, sizeof g_avp[i]);
        g_avp[i].used = 1;
        if (got >= sizeof w) { g_avp[i].event_obj = (void*)(uintptr_t)w[10]; g_avp[i].event_cb = (AvpEventCb)(uintptr_t)w[11]; }
        uint64_t h = AVP_HANDLE_BASE + (uint64_t)i + 1;
        rt_logf("sceAvPlayerInit -> player handle %#" PRIx64 " (opaque)", h);
        return h;
    }
    rt_logf("sceAvPlayerInit: all %d player slots are in use", AVP_MAX);
    return 0;
}

// Delivers READY on a thread of its own, as the real library does from its controller thread. The short delay keeps the order
// the game sees on a console, where AddSource returns before the asynchronous open completes.
static void* GUEST_ABI avp_ready_thread(void* arg) {
    uint64_t h = (uint64_t)(uintptr_t)arg;
    struct timespec ts = {0, 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
    AvpPlayer* p = avp_get(h);
    if (!p || !p->event_cb) return NULL;
    rt_logf("sceAvPlayer: delivering event READY (2) for '%s' to the game's callback %p (object %p). No decoder: the source has ZERO streams",
            p->path, (void*)p->event_cb, p->event_obj);
    p->event_cb(p->event_obj, AVP_EVENT_READY, 0, NULL);
    rt_logf("sceAvPlayer: the game's READY callback returned");
    return NULL;
}
static int GUEST_ABI avp_add_source(uint64_t h, const char* path) {
    AvpPlayer* p = avp_get(h);
    if (!p || !path) return AVP_ERR_INVALID_PARAMS;
    char name[512]; size_t got = os_safe_read(name, path, sizeof name - 1);
    name[got] = 0; name[sizeof name - 1] = 0;
    size_t len = strnlen(name, sizeof name - 1); name[len] = 0;
    snprintf(p->path, sizeof p->path, "%s", name);
    p->sources++; p->file_size = 0; p->brand[0] = 0;
    char host[1536];
    int e = rt_fs_host_path(name, host, sizeof host);
    FILE* f = e ? NULL : os_fopen(host, "rb");
    if (f) {
        uint8_t hdr[16] = {0};
        size_t r = fread(hdr, 1, sizeof hdr, f);
        fseek(f, 0, SEEK_END); p->file_size = (uint64_t)ftell(f);
        fclose(f);
        if (r >= 12 && !memcmp(hdr + 4, "ftyp", 4)) { memcpy(p->brand, hdr + 8, 4); p->brand[4] = 0; }
        rt_logf("sceAvPlayerAddSource('%s'): file exists, %" PRIu64 " bytes, MP4 brand '%s'. NOT DECODED: the movie is reported as having no streams and ends immediately",
                name, p->file_size, p->brand[0] ? p->brand : "(not an MP4 ftyp header)");
    } else {
        rt_logf("sceAvPlayerAddSource('%s'): file cannot be opened (%s); a source is still accepted. NOT DECODED either way", name, e ? "path does not resolve under /app0" : "open failed");
    }
    if (p->event_cb) {
        if (rt_spawn_guest_thread(avp_ready_thread, (void*)(uintptr_t)h, "AvPlayer Event")) rt_logf("sceAvPlayerAddSource: could not start the event thread; READY will not be delivered");
    } else rt_logf("sceAvPlayerAddSource: the game gave no event callback; no READY event is delivered");
    return 0;
}
static int GUEST_ABI avp_stream_count(uint64_t h) { return avp_get(h) ? 0 : AVP_ERR_INVALID_PARAMS; }
static int GUEST_ABI avp_get_stream_info(uint64_t h, uint32_t idx, void* info) {
    (void)info; (void)idx;
    return avp_get(h) ? AVP_ERR_INVALID_PARAMS /* there are no streams, so no index is valid */ : AVP_ERR_INVALID_PARAMS;
}
static int GUEST_ABI avp_enable_stream(uint64_t h, uint32_t idx) { (void)idx; return avp_get(h) ? AVP_ERR_INVALID_PARAMS : AVP_ERR_INVALID_PARAMS; }
static int GUEST_ABI avp_start(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    p->started = 1;
    rt_logf("sceAvPlayerStart('%s'): nothing is played (no decoder)", p->path);
    return 0;
}
static int GUEST_ABI avp_stop(uint64_t h) { AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS; p->started = 0; return 0; }
static int GUEST_ABI avp_set_looping(uint64_t h, int loop) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    p->looping = loop & 1;
    if (loop) rt_logf("sceAvPlayerSetLooping(true) for '%s': recorded; with no stream the player is still inactive", p->path);
    return 0;
}
static int GUEST_ABI avp_is_active(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return 0;
    if (p->calls_active++ == 0) rt_logf("sceAvPlayerIsActive('%s'): false (no decoder: movie treated as finished)", p->path);
    return 0;
}
static int GUEST_ABI avp_get_video_data_ex(uint64_t h, void* info) {
    (void)info;
    AvpPlayer* p = avp_get(h); if (!p) return 0;
    if (p->calls_video++ == 0) rt_logf("sceAvPlayerGetVideoDataEx('%s'): no frame (no decoder)", p->path);
    return 0;
}
static int GUEST_ABI avp_close(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    rt_logf("sceAvPlayerClose('%s'): %d source(s) were added, %" PRIu64 " IsActive and %" PRIu64 " GetVideoDataEx polls", p->path, p->sources, p->calls_active, p->calls_video);
    p->used = 0;
    return 0;
}

const HostSymbol avplayer_symbols[] = {
    {"sceAvPlayerInit", "libSceAvPlayer", (void*)avp_init, 0},
    {"sceAvPlayerAddSource", "libSceAvPlayer", (void*)avp_add_source, 0},
    {"sceAvPlayerStreamCount", "libSceAvPlayer", (void*)avp_stream_count, 0},
    {"sceAvPlayerGetStreamInfo", "libSceAvPlayer", (void*)avp_get_stream_info, 0},
    {"sceAvPlayerEnableStream", "libSceAvPlayer", (void*)avp_enable_stream, 0},
    {"sceAvPlayerStart", "libSceAvPlayer", (void*)avp_start, 0},
    {"sceAvPlayerStop", "libSceAvPlayer", (void*)avp_stop, 0},
    {"sceAvPlayerSetLooping", "libSceAvPlayer", (void*)avp_set_looping, 0},
    {"sceAvPlayerIsActive", "libSceAvPlayer", (void*)avp_is_active, 0},
    {"sceAvPlayerGetVideoDataEx", "libSceAvPlayer", (void*)avp_get_video_data_ex, 0},
    {"sceAvPlayerClose", "libSceAvPlayer", (void*)avp_close, 0},
};
const size_t avplayer_symbol_count = sizeof avplayer_symbols / sizeof *avplayer_symbols;

// Self-test of the honest no-decoder behavior (host tests; no game files needed).
int GUEST_ABI rt_avplayer_selftest(int (GUEST_ABI* check)(int ok, const char* what)) {
    int bad = 0;
#define C(ok, what) do { if (!check((ok), (what))) ++bad; } while (0)
    C(avp_init(NULL) == 0, "avplayer: Init(NULL) is rejected");
    uint64_t init[16] = {0};
    uint64_t h = avp_init(init);
    C(h > AVP_HANDLE_BASE, "avplayer: Init returns a handle");
    C(avp_add_source(h + 100, "/app0/x.mp4") == AVP_ERR_INVALID_PARAMS, "avplayer: AddSource on an unknown handle is INVALID_PARAMS");
    C(avp_add_source(h, "/app0/does-not-exist.mp4") == 0, "avplayer: AddSource accepts a source that cannot be opened (logged)");
    C(avp_stream_count(h) == 0, "avplayer: no streams are reported (no decoder)");
    C(avp_start(h) == 0 && avp_is_active(h) == 0, "avplayer: after Start the player is not active (no decoder)");
    C(avp_get_video_data_ex(h, NULL) == 0, "avplayer: GetVideoDataEx never returns a frame");
    C(avp_close(h) == 0 && avp_close(h) == AVP_ERR_INVALID_PARAMS, "avplayer: Close once, then the handle is invalid");
#undef C
    return bad;
}
