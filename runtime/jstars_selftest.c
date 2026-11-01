// --selftest-host: checks the runtime's own host services without loading the game. A pass is what
// earns the "verified" audit status of the functions named in jstars_audit.c; each check prints its
// name. Runs the same way on the Linux harness and on Windows.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void* rt_find_host(const char* name);
static int g_fail, g_checks;
#define CHECK(cond, what) do { ++g_checks; if (!(cond)) { ++g_fail; rt_logf("SELFTEST FAIL: %s (%s:%d)", what, __FILE__, __LINE__); } else rt_logf("selftest ok: %s", what); } while (0)

static int once_hits;
static void GUEST_ABI once_fn(void) { ++once_hits; }
typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } RtcDateTime;

static void* g_cm; static void* g_cc; static volatile int g_flag, g_woken;
static void* GUEST_ABI thread_main(void* arg) {
    void* (GUEST_ABI * dummy)(void*) = 0; (void)dummy;
    *(int*)arg += 1;
    return (void*)(uintptr_t)0x1234;
}
static void* GUEST_ABI thread_exit_value(void* arg) {
    (void)arg;
    ((void(GUEST_ABI*)(void*))rt_find_host("scePthreadExit"))((void*)(uintptr_t)0x77);
    return NULL;
}
static void* GUEST_ABI thread_wait(void* arg) {
    (void)arg;
    ((int(GUEST_ABI*)(void**))rt_find_host("scePthreadMutexLock"))(&g_cm);
    while (!g_flag) ((int(GUEST_ABI*)(void**, void**))rt_find_host("scePthreadCondWait"))(&g_cc, &g_cm);
    g_woken = 1;
    ((int(GUEST_ABI*)(void**))rt_find_host("scePthreadMutexUnlock"))(&g_cm);
    return NULL;
}

extern int GUEST_ABI rt_fs_selftest(const char* scratch_dir, int (GUEST_ABI* check)(int ok, const char* what));
static int GUEST_ABI fs_check(int ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; rt_logf("SELFTEST FAIL: %s", what); } else rt_logf("selftest ok: %s", what); return ok; }
extern int GUEST_ABI rt_savedata_selftest(int (GUEST_ABI* check)(int ok, const char* what));
extern int GUEST_ABI rt_avplayer_selftest(int (GUEST_ABI* check)(int ok, const char* what));

static void* g_ef; static volatile int g_ef_phase;
static void* GUEST_ABI evf_waiter(void* arg) {
    (void)arg;
    uint64_t res = 0;
    int r = ((int(GUEST_ABI*)(void*, uint64_t, uint32_t, uint64_t*, uint32_t*))rt_find_host("sceKernelWaitEventFlag"))(g_ef, 0x6, 0x11, &res, NULL); // AND, clear all
    g_ef_phase = (r == 0 && res == 0x7) ? 1 : -1;
    return NULL;
}

int rt_selftest_host(const char* scratch_dir) {
    rt_thread_attach_main();
#define F(ret, name, args) ((ret(GUEST_ABI*) args)rt_find_host(name))
    void* m = NULL; void* c = NULL; void* rw = NULL;
    CHECK(F(int, "scePthreadMutexInit", (void**, void*, const char*))(&m, NULL, "t") == 0, "mutex init");
    CHECK(F(int, "scePthreadMutexLock", (void**))(&m) == 0, "mutex lock");
    CHECK(F(int, "scePthreadMutexTrylock", (void**))(&m) != 0, "trylock on a held mutex fails");
    CHECK(F(int, "scePthreadMutexUnlock", (void**))(&m) == 0, "mutex unlock");
    CHECK(F(int, "scePthreadMutexTrylock", (void**))(&m) == 0, "trylock on a free mutex succeeds");
    F(int, "scePthreadMutexUnlock", (void**))(&m);
    CHECK(F(int, "scePthreadCondInit", (void**, void*, const char*))(&c, NULL, "t") == 0, "cond init");
    F(int, "scePthreadMutexLock", (void**))(&m);
    uint64_t t0 = F(uint64_t, "sceKernelGetProcessTime", (void))();
    int r = F(int, "scePthreadCondTimedwait", (void**, void**, uint32_t))(&c, &m, 20000);
    uint64_t t1 = F(uint64_t, "sceKernelGetProcessTime", (void))();
    F(int, "scePthreadMutexUnlock", (void**))(&m);
    CHECK(r == (int)0x80020000 + ETIMEDOUT || r == (int)0x8002003c, "cond timed wait times out with the SCE ETIMEDOUT code");
    CHECK(t1 - t0 >= 19000 && t1 - t0 < 400000, "cond timed wait lasts about the requested 20 ms");
    CHECK(F(int, "scePthreadCondSignal", (void**))(&c) == 0, "cond signal without waiters");
    CHECK(F(int, "scePthreadRwlockInit", (void**, void*, const char*))(&rw, NULL, "t") == 0, "rwlock init");
    CHECK(F(int, "scePthreadRwlockRdlock", (void**))(&rw) == 0 && F(int, "scePthreadRwlockUnlock", (void**))(&rw) == 0, "rwlock read lock/unlock");
    uint32_t key = 0; int token = 7;
    CHECK(F(int, "scePthreadKeyCreate", (uint32_t*, void*))(&key, NULL) == 0, "key create");
    CHECK(F(int, "scePthreadSetspecific", (uint32_t, const void*))(key, &token) == 0 && F(void*, "scePthreadGetspecific", (uint32_t))(key) == &token, "key set/get round trip");
    int ctl = 0;
    F(int, "scePthreadOnce", (int*, void*))(&ctl, (void*)once_fn);
    F(int, "scePthreadOnce", (int*, void*))(&ctl, (void*)once_fn);
    CHECK(once_hits == 1, "once runs its function exactly once");
    struct timespec a, b;
    CHECK(F(int, "clock_gettime", (int, struct timespec*))(4, &a) == 0 || F(int, "clock_gettime", (int, struct timespec*))(1, &a) == 0, "clock_gettime monotonic");
    F(int, "sceKernelUsleep", (uint32_t))(15000);
    F(int, "clock_gettime", (int, struct timespec*))(4, &b);
    long long dns = (long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec);
    CHECK(dns >= 14000000ll && dns < 300000000ll, "sceKernelUsleep sleeps at least the requested time");
    CHECK(F(uint64_t, "sceKernelGetProcessTime", (void))() >= t1, "process time is monotonic");
    int64_t phys = -1;
    CHECK(F(int, "sceKernelAllocateDirectMemory", (uint64_t, uint64_t, uint64_t, uint64_t, int, int64_t*))(0, 0, 0x10000, 0, 0, &phys) == 0 && phys >= 0, "direct memory extent allocation");
    CHECK(F(int, "sceKernelReleaseDirectMemory", (uint64_t, uint64_t))((uint64_t)phys, 0x10000) == 0, "direct memory release of the same extent");
    CHECK(*F(int*, "__error", (void))() >= 0, "errno cell reachable");
    CHECK(F(int, "getpagesize", (void))() == 16384, "page size is the console's 16384");
    RtcDateTime dt; uint64_t tick = 0;
    F(int, "sceRtcGetCurrentTick", (uint64_t*))(&tick);
    CHECK(F(int, "sceRtcSetTick", (RtcDateTime*, const uint64_t*))(&dt, &tick) == 0 && dt.year >= 2024, "rtc tick -> date");
    uint64_t known = 62135596800000000ull + 1000000ull * 86400ull * 19000ull; // 2022-01-08 00:00:00 UTC (day 19000 since 1970)
    F(int, "sceRtcSetTick", (RtcDateTime*, const uint64_t*))(&dt, &known);
    CHECK(dt.year == 2022 && dt.month == 1 && dt.day == 8, "rtc known tick converts to 2022-01-08");
    CHECK(F(int, "sceRtcGetDayOfWeek", (int, int, int))(2022, 1, 8) == 6, "2022-01-08 is a Saturday");

    // ---- attributes, threads ----
    void* attr = NULL; void* th = NULL; int touched = 0; void* retv = NULL;
    CHECK(F(int, "scePthreadAttrInit", (void**))(&attr) == 0, "thread attr init");
    CHECK(F(int, "scePthreadAttrSetstacksize", (void**, size_t))(&attr, 256 * 1024) == 0, "thread attr stack size");
    CHECK(F(int, "scePthreadAttrSetdetachstate", (void**, int))(&attr, 0) == 0, "thread attr joinable");
    CHECK(F(int, "scePthreadCreate", (void**, void**, void*, void*, const char*))(&th, &attr, (void*)thread_main, &touched, "st-a") == 0, "thread create");
    CHECK(F(int, "scePthreadJoin", (void*, void**))(th, &retv) == 0 && touched == 1 && retv == (void*)(uintptr_t)0x1234, "thread join returns the entry's value and ran the entry");
    th = NULL;
    CHECK(F(int, "scePthreadCreate", (void**, void**, void*, void*, const char*))(&th, &attr, (void*)thread_exit_value, NULL, "st-b") == 0, "thread create (exit)");
    retv = NULL;
    CHECK(F(int, "scePthreadJoin", (void*, void**))(th, &retv) == 0 && retv == (void*)(uintptr_t)0x77, "scePthreadExit value reaches join");
    void* self = F(void*, "scePthreadSelf", (void))();
    CHECK(self != NULL && F(int, "scePthreadEqual", (void*, void*))(self, self) != 0 && F(int, "scePthreadEqual", (void*, void*))(self, th) == 0, "self/equal");
    th = NULL;
    void* dattr = NULL;
    F(int, "scePthreadAttrInit", (void**))(&dattr);
    F(int, "scePthreadAttrSetdetachstate", (void**, int))(&dattr, 1);
    CHECK(F(int, "scePthreadCreate", (void**, void**, void*, void*, const char*))(&th, &dattr, (void*)thread_main, &touched, "st-c") == 0, "detached thread create");
    F(int, "scePthreadYield", (void))();
    CHECK(F(int, "scePthreadAttrDestroy", (void**))(&dattr) == 0 && F(int, "scePthreadAttrDestroy", (void**))(&attr) == 0, "thread attr destroy");
    CHECK(F(int, "sched_yield", (void))() == 0, "sched_yield");
    // ---- mutex attributes (recursive), destroy ----
    void* ma = NULL; void* rm = NULL;
    CHECK(F(int, "scePthreadMutexattrInit", (void**))(&ma) == 0 && F(int, "scePthreadMutexattrSettype", (void**, int))(&ma, 2) == 0, "mutex attr recursive");
    F(int, "scePthreadMutexInit", (void**, void*, const char*))(&rm, &ma, "rec");
    CHECK(F(int, "scePthreadMutexLock", (void**))(&rm) == 0 && F(int, "scePthreadMutexLock", (void**))(&rm) == 0, "recursive mutex locks twice");
    F(int, "scePthreadMutexUnlock", (void**))(&rm); F(int, "scePthreadMutexUnlock", (void**))(&rm);
    CHECK(F(int, "scePthreadMutexDestroy", (void**))(&rm) == 0 && F(int, "scePthreadMutexattrDestroy", (void**))(&ma) == 0, "mutex and attr destroy");
    // ---- the game's libc writes inside the objects the slots point to (libc.prx+0x23a20: or dword [obj+0x20], 0x100) ----
    {
        void* pm = NULL; void* pc = NULL; void* prw = NULL; void* pa = NULL;
        F(int, "scePthreadMutexInit", (void**, void*, const char*))(&pm, NULL, "poke");
        F(int, "scePthreadCondInit", (void**, void*, const char*))(&pc, NULL, "poke");
        F(int, "scePthreadRwlockInit", (void**, void*, const char*))(&prw, NULL, "poke");
        F(int, "scePthreadAttrInit", (void**))(&pa);
        // every byte of the first 0x100 bytes of each object belongs to the guest: scribble over all of it
        memset(pm, 0xA5, 0x100); memset(pc, 0xA5, 0x100); memset(prw, 0xA5, 0x100); memset(pa, 0, 0x100);
        *(uint32_t*)((char*)pm + 0x20) |= 0x100;
        CHECK(F(int, "scePthreadMutexLock", (void**))(&pm) == 0 && F(int, "scePthreadMutexUnlock", (void**))(&pm) == 0, "mutex still works after the guest wrote into its object");
        CHECK(F(int, "scePthreadCondSignal", (void**))(&pc) == 0, "cond still works after the guest wrote into its object");
        CHECK(F(int, "scePthreadRwlockRdlock", (void**))(&prw) == 0 && F(int, "scePthreadRwlockUnlock", (void**))(&prw) == 0, "rwlock still works after the guest wrote into its object");
        CHECK(F(int, "scePthreadMutexDestroy", (void**))(&pm) == 0 && F(int, "scePthreadCondDestroy", (void**))(&pc) == 0 && F(int, "scePthreadRwlockDestroy", (void**))(&prw) == 0, "poked objects destroy cleanly");
        free(pa);
    }
    // ---- cond wait / broadcast wake-up between threads ----
    g_cm = NULL; g_cc = NULL; g_flag = 0; g_woken = 0;
    F(int, "scePthreadMutexInit", (void**, void*, const char*))(&g_cm, NULL, "w");
    F(int, "scePthreadCondInit", (void**, void*, const char*))(&g_cc, NULL, "w");
    void* wt = NULL;
    F(int, "scePthreadCreate", (void**, void**, void*, void*, const char*))(&wt, NULL, (void*)thread_wait, NULL, "st-wait");
    F(int, "sceKernelUsleep", (uint32_t))(30000);
    F(int, "scePthreadMutexLock", (void**))(&g_cm); g_flag = 1; F(int, "scePthreadCondBroadcast", (void**))(&g_cc); F(int, "scePthreadMutexUnlock", (void**))(&g_cm);
    F(int, "scePthreadJoin", (void*, void**))(wt, NULL);
    CHECK(g_woken == 1, "cond broadcast wakes a waiting thread");
    CHECK(F(int, "scePthreadCondDestroy", (void**))(&g_cc) == 0 && F(int, "scePthreadMutexDestroy", (void**))(&g_cm) == 0, "cond destroy");
    // ---- rwlock write / try / destroy ----
    CHECK(F(int, "scePthreadRwlockWrlock", (void**))(&rw) == 0 && F(int, "scePthreadRwlockTryrdlock", (void**))(&rw) != 0 && F(int, "scePthreadRwlockUnlock", (void**))(&rw) == 0, "rwlock write lock excludes readers");
    CHECK(F(int, "scePthreadRwlockTrywrlock", (void**))(&rw) == 0 && F(int, "scePthreadRwlockUnlock", (void**))(&rw) == 0 && F(int, "scePthreadRwlockDestroy", (void**))(&rw) == 0, "rwlock trywrlock/destroy");
    CHECK(F(int, "scePthreadKeyDelete", (uint32_t))(key) == 0, "key delete");
    // ---- clocks ----
    struct timespec rt; struct { int64_t s, us; } tv; struct timespec rem = {0, 0}, req = {0, 5000000};
    CHECK(F(int, "clock_gettime", (int, struct timespec*))(0, &rt) == 0 && rt.tv_sec > 1700000000, "clock_gettime realtime is after 2023");
    CHECK(F(int, "sceKernelClockGettime", (int, struct timespec*))(4, &a) == 0, "sceKernelClockGettime monotonic");
    CHECK(F(int, "gettimeofday", (void*, void*))(&tv, NULL) == 0 && tv.s > 1700000000, "gettimeofday");
    CHECK(F(int, "sceKernelGettimeofday", (void*))(&tv) == 0 && tv.s > 1700000000, "sceKernelGettimeofday");
    F(int, "clock_gettime", (int, struct timespec*))(4, &a);
    CHECK(F(int, "nanosleep", (const struct timespec*, struct timespec*))(&req, &rem) == 0, "nanosleep returns");
    F(int, "clock_gettime", (int, struct timespec*))(4, &b);
    CHECK((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec) >= 4500000ll, "nanosleep sleeps at least the requested time");
    CHECK(F(int, "getpid", (void))() == 1, "getpid is the fixed pid 1");

    // ---- event flags ----
    void* ef = NULL; uint64_t res = 0; uint32_t to = 20000;
    CHECK(F(int, "sceKernelCreateEventFlag", (void**, const char*, uint32_t, uint64_t, const void*))(&ef, "st", 0x21, 0x1, NULL) == 0, "event flag create (multi)");
    CHECK(F(int, "sceKernelPollEventFlag", (void*, uint64_t, uint32_t, uint64_t*))(ef, 0x2, 0x1, &res) == (int)0x80020010, "poll of an unmet pattern is EBUSY");
    CHECK(F(int, "sceKernelWaitEventFlag", (void*, uint64_t, uint32_t, uint64_t*, uint32_t*))(ef, 0x2, 0x1, &res, &to) == (int)0x8002003c && to == 0, "timed wait times out with ETIMEDOUT and zeroes the timeout");
    CHECK(F(int, "sceKernelWaitEventFlag", (void*, uint64_t, uint32_t, uint64_t*, uint32_t*))(ef, 0x1, 0x2, &res, NULL) == 0 && res == 0x1, "wait on an already-set bit returns at once with the pattern");
    CHECK(F(int, "sceKernelWaitEventFlag", (void*, uint64_t, uint32_t, uint64_t*, uint32_t*))(ef, 0x1, 0x22, &res, NULL) == 0, "wait with clear-pattern");
    CHECK(F(int, "sceKernelPollEventFlag", (void*, uint64_t, uint32_t, uint64_t*))(ef, 0x1, 0x2, &res) == (int)0x80020010, "clear-pattern consumed the bit");
    CHECK(F(int, "sceKernelWaitEventFlag", (void*, uint64_t, uint32_t, uint64_t*, uint32_t*))(ef, 0, 0x1, &res, NULL) == (int)0x80020016, "wait with an empty pattern is EINVAL");
    g_ef = ef; g_ef_phase = 0;
    void* wt2 = NULL;
    F(int, "scePthreadCreate", (void**, void**, void*, void*, const char*))(&wt2, NULL, (void*)evf_waiter, NULL, "st-evf");
    F(int, "sceKernelUsleep", (uint32_t))(30000);
    F(int, "sceKernelSetEventFlag", (void*, uint64_t))(ef, 0x2);
    F(int, "sceKernelUsleep", (uint32_t))(20000);
    CHECK(g_ef_phase == 0, "an AND waiter is not woken by half of its pattern");
    F(int, "sceKernelSetEventFlag", (void*, uint64_t))(ef, 0x5);
    F(int, "scePthreadJoin", (void*, void**))(wt2, NULL);
    CHECK(g_ef_phase == 1, "an AND waiter is woken when its whole pattern is set and sees the full pattern");
    CHECK(F(int, "sceKernelPollEventFlag", (void*, uint64_t, uint32_t, uint64_t*))(ef, 0x7, 0x2, &res) == (int)0x80020010, "the clear-all wake-up emptied the flag");
    F(int, "sceKernelSetEventFlag", (void*, uint64_t))(ef, 0xff);
    F(int, "sceKernelClearEventFlag", (void*, uint64_t))(ef, 0x0f);
    CHECK(F(int, "sceKernelPollEventFlag", (void*, uint64_t, uint32_t, uint64_t*))(ef, 0xff, 0x2, &res) == 0 && res == 0x0f, "clear ANDs the flag with its argument");
    int nw = -1;
    CHECK(F(int, "sceKernelCancelEventFlag", (void*, uint64_t, int*))(ef, 0x3, &nw) == 0 && nw == 0, "cancel with no waiters reports 0");
    CHECK(F(int, "sceKernelDeleteEventFlag", (void*))(ef) == 0, "event flag delete");
    CHECK(F(int, "sceKernelSetEventFlag", (void*, uint64_t))(NULL, 1) == (int)0x80020003, "operations on a null flag are ESRCH");
    // ---- signal masks, time zone ----
    uint32_t full[4], old[4] = {7, 7, 7, 7}, one[4] = {1, 0, 0, 0};
    F(int, "sigfillset", (void*))(full);
    CHECK(full[0] == 0xffffffffu && full[3] == 0xffffffffu, "sigfillset fills the 16-byte set");
    F(int, "sigprocmask", (int, const void*, void*))(3, one, NULL);
    F(int, "sigprocmask", (int, const void*, void*))(1, full, old);
    CHECK(old[0] == 1 && old[1] == 0, "sigprocmask returns the previous mask");
    F(int, "sigprocmask", (int, const void*, void*))(3, one, NULL);
    struct { int32_t west, dst; } tz;
    CHECK(F(int, "sceKernelGettimezone", (void*))(&tz) == 0 && tz.west > -900 && tz.west < 900, "time zone offset is plausible");
    // ---- file system ----
    int fsbad = rt_fs_selftest(scratch_dir, fs_check);
    fsbad += rt_savedata_selftest(fs_check);
    fsbad += rt_avplayer_selftest(fs_check);
    // ---- audio: AudioOut meters/converts what the game submits and --dump-audio records it as a valid stereo S16 WAV ----
    {
        char wp[4096], wf[4200]; snprintf(wp, sizeof wp, "%s/audio-dump", scratch_dir); snprintf(wf, sizeof wf, "%s.port1.wav", wp);
        rt_audio_set_device_enabled(0); rt_audio_set_dump(wp);
        int h = F(int, "sceAudioOutOpen", (int, int, int, uint32_t, uint32_t, uint32_t))(0, 0, 0, 256, 48000, 4); // float stereo, null sink
        CHECK(h == 1, "AudioOut: a float-stereo port opens as handle 1");
        float buf[256 * 2];
        for (int i = 0; i < 256; ++i) { buf[2 * i] = 0.5f * sinf(0.05f * (float)i); buf[2 * i + 1] = -0.25f; }
        int outs = 0;
        for (int k = 0; k < 4; ++k) outs += F(int, "sceAudioOutOutput", (int, const void*))(h, buf) == 256;
        CHECK(outs == 4, "AudioOut: Output returns the frame count");
        CHECK(F(int, "sceAudioOutClose", (int))(h) == 0, "AudioOut: close succeeds");
        rt_audio_set_dump("");
        FILE* wv = fopen(wf, "rb"); unsigned char hd[44] = {0}; long sz = -1; int16_t smp[4] = {0};
        if (wv) { fread(hd, 1, 44, wv); fread(smp, 2, 4, wv); fseek(wv, 0, SEEK_END); sz = ftell(wv); fclose(wv); }
        uint32_t dsz = hd[40] | hd[41] << 8 | hd[42] << 16 | (uint32_t)hd[43] << 24, rate = hd[24] | hd[25] << 8 | hd[26] << 16 | (uint32_t)hd[27] << 24;
        CHECK(wv && !memcmp(hd, "RIFF", 4) && !memcmp(hd + 8, "WAVEfmt ", 8) && !memcmp(hd + 36, "data", 4), "audio dump: the WAV header is well formed");
        CHECK(sz == 44 + 4 * 256 * 4 && dsz == 4 * 256 * 4 && rate == 48000, "audio dump: four 256-frame buffers = 4096 bytes of 48 kHz stereo S16 and the header says so");
        CHECK(smp[1] == (int16_t)(-0.25f * 32767.0f) && smp[2] > 800 && smp[2] < 820, "audio dump: samples are the submitted ones (right channel -0.25, left follows the sine)");
    }
    // ---- audio: the 7.1 -> stereo downmix is soft-limited instead of hard-clipped (observed downmix peak 1.04 in the first battle) ----
    {
        char wp[4096], wf[4200]; snprintf(wp, sizeof wp, "%s/audio-limit", scratch_dir); snprintf(wf, sizeof wf, "%s.port1.wav", wp);
        rt_audio_set_device_enabled(0); rt_audio_set_dump(wp);
        int h = F(int, "sceAudioOutOpen", (int, int, int, uint32_t, uint32_t, uint32_t))(0, 0, 0, 256, 48000, 5); // float 8ch, null sink
        float buf[256 * 8];
        for (int i = 0; i < 256; ++i) { for (int j = 0; j < 8; ++j) buf[8 * i + j] = 0.0f; buf[8 * i] = 0.6f; buf[8 * i + 2] = 0.2f; buf[8 * i + 4] = 0.2f; buf[8 * i + 6] = 0.2f; } // downmix sum 0.6 + 3 * 0.707 * 0.2 = 1.024
        for (int k = 0; k < 2; ++k) F(int, "sceAudioOutOutput", (int, const void*))(h, buf);
        F(int, "sceAudioOutClose", (int))(h);
        rt_audio_set_dump("");
        FILE* wv = fopen(wf, "rb"); int16_t smp[4] = {0};
        if (wv) { fseek(wv, 44, SEEK_SET); fread(smp, 2, 4, wv); fclose(wv); }
        CHECK(wv && smp[0] > 31000 && smp[0] < 32700, "audio limiter: a downmix sum of 1.024 is soft-limited to about 0.985 of full scale (a hard clip would give 32767)");
    }
    // ---- pad: scripted test input drives scePadReadState with the SDK layout ----
    {
        char sp[4096]; snprintf(sp, sizeof sp, "%s/pad-script.txt", scratch_dir);
        FILE* sf = fopen(sp, "w");
        if (sf) { fputs("# selftest\nat 0.00 press CROSS for 100000\nat 0.00 stick left 0 255 for 100000\nat 0.00 press L2+UP for 100000\nat 500 press CIRCLE\n"
                        "every 200 from 1000 to 2000 press OPTIONS for 50\nbogus line\n", sf); fclose(sf); }
        CHECK(rt_input_load_script(sp) != 0, "pad script: unrecognised lines are reported (load returns an error) but valid lines are kept");
        int h = F(int, "scePadOpen", (int, int, int, const void*))(1, 0, 0, NULL);
        struct { uint32_t buttons; uint8_t left[2], right[2]; uint8_t l2, r2, pad2[2]; float o[4], a[3], g[3]; uint8_t touch[28]; uint8_t connected; } *pd = calloc(1, 256);
        int rr = F(int, "scePadReadState", (int, void*))(h, pd);
        CHECK(h == 1 && rr == 0, "pad: open returns handle 1 and read succeeds");
        CHECK((pd->buttons & 0x4000) && (pd->buttons & 0x10) && (pd->buttons & 0x100) && !(pd->buttons & 0x2000), "pad script: CROSS+UP+L2 held, CIRCLE not yet (console button bits)");
        CHECK(pd->left[0] == 0 && pd->left[1] == 255 && pd->right[0] == 128 && pd->l2 == 255, "pad script: left stick (0,255), right stick centred, L2 analog 255");
        free(pd);
    }
    // ---- trophies: the registered set is empty, so nothing can unlock (first reached in the first battle) ----
    {
        int ctx = 0, th = 0, plat = 0x1234;
        CHECK(F(int, "sceNpTrophyCreateContext", (int*, int, uint32_t, uint64_t))(&ctx, 1, 0, 0) == 0 && ctx >= 1, "trophy: context created (bookkeeping only)");
        CHECK(F(int, "sceNpTrophyCreateHandle", (int*))(&th) == 0 && th >= 1, "trophy: handle created");
        CHECK(F(int, "sceNpTrophyUnlockTrophy", (int, int, int, int*))(ctx, th, 1, &plat) == (int)0x8055160F, "trophy: unlock before RegisterContext reports NOT_REGISTERED");
        CHECK(F(int, "sceNpTrophyRegisterContext", (int, int, uint64_t))(ctx, th, 0) == 0, "trophy: RegisterContext registers the EMPTY set (logged as limited support)");
        CHECK(F(int, "sceNpTrophyUnlockTrophy", (int, int, int, int*))(ctx, th, 1, &plat) == (int)0x8055160A && plat == 0x1234, "trophy: unlock answers INVALID_TROPHY_ID (empty set) and leaves platinumId untouched");
        CHECK(F(int, "sceNpTrophyUnlockTrophy", (int, int, int, int*))(ctx, th, 1, NULL) == (int)0x80551604, "trophy: unlock with a NULL platinumId is an INVALID_ARGUMENT");
        CHECK(F(int, "sceNpTrophyUnlockTrophy", (int, int, int, int*))(99, th, 1, &plat) == (int)0x80551609, "trophy: unlock with a bad context is INVALID_CONTEXT");
        F(int, "sceNpTrophyDestroyHandle", (int))(th); F(int, "sceNpTrophyDestroyContext", (int))(ctx);
    }
    // ---- network (jstars_net.c) ----------------------------------------------------------------------------
    // The default is offline, so the first group checks the offline answers; then the layer is switched on and the
    // sockets are exercised against the loopback interface. Nothing contacts any outside host.
    {
        typedef struct { uint8_t len, family; uint16_t port, pad_hi; uint16_t vport; char zero[6]; } SelfAddrProbe;
        (void)sizeof(SelfAddrProbe);
        struct GuestAddr { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; char zero[6]; } a, from;
        int* err = F(int*, "sceNetErrnoLoc", (void))();
        CHECK(err != NULL, "net: sceNetErrnoLoc returns a per-thread errno cell");
        CHECK(F(uint16_t, "sceNetHtons", (uint16_t))(0x1234) == 0x3412, "net: htons swaps byte order");
        CHECK(F(int, "sceNetInit", (void))() == 0, "net: library initializes");

        rt_net_configure(0, 0);
        CHECK(F(int, "sceNetSocket", (const char*, int, int, int))("off", 2, 2, 17) == -1 && *err == 50,
              "net: offline (no --online) socket creation reports ENETDOWN");
        int st = -1;
        CHECK(F(int, "sceNetCtlGetState", (int*))(&st) == 0 && st == 0, "net: offline NetCtl reports DISCONNECTED");

        rt_net_configure(1, 0);
        int s = F(int, "sceNetSocket", (const char*, int, int, int))("selftest", 2, 2, 17);
        CHECK(s >= 1, "net: online datagram socket is created");
        CHECK(F(int, "sceNetSocket", (const char*, int, int, int))("bad-family", 28, 2, 17) == -1 && *err == 47,
              "net: an unsupported address family reports EAFNOSUPPORT");
        CHECK(F(int, "sceNetSocket", (const char*, int, int, int))("stream", 2, 1, 6) == -1 && *err == 43,
              "net: a stream socket reports EPROTONOSUPPORT (only datagram sockets exist)");
        CHECK(F(int, "sceNetBind", (int, const void*, int))(9999, &a, sizeof a) == -1 && *err == 9,
              "net: an operation on an unknown socket reports EBADF");

        memset(&a, 0, sizeof a);
        a.len = sizeof a; a.family = 2; a.port = 0; a.addr = F(uint32_t, "sceNetHtonl", (uint32_t))(0x7f000001); // 127.0.0.1
        CHECK(F(int, "sceNetBind", (int, const void*, int))(s, &a, sizeof a) == 0, "net: socket binds to the loopback address");
        int nbio = 1;
        CHECK(F(int, "sceNetSetsockopt", (int, int, int, const void*, int))(s, 0xFFFF, 0x1200, &nbio, sizeof nbio) == 0,
              "net: SO_NBIO is accepted");
        char buf[64];
        CHECK(F(int, "sceNetRecvfrom", (int, void*, size_t, int, void*, int*))(s, buf, sizeof buf, 0, NULL, NULL) == -1 && *err == 35,
              "net: a non-blocking receive with nothing queued reports EAGAIN (the console's value 35)");

        // Send to this socket's own address and read the datagram back, which also checks address translation.
        int alen = sizeof a;
        struct GuestAddr self_addr;
        memset(&self_addr, 0, sizeof self_addr);
        {
            // sceNetBind with port 0 leaves the real port to the host, so ask a second socket to talk to this one by
            // re-binding a known port instead: port 0 cannot be addressed.
            int s2 = F(int, "sceNetSocket", (const char*, int, int, int))("selftest-peer", 2, 2, 17);
            CHECK(s2 >= 1, "net: a second socket is created");
            struct GuestAddr fixed;
            memset(&fixed, 0, sizeof fixed);
            fixed.len = sizeof fixed; fixed.family = 2;
            fixed.port = F(uint16_t, "sceNetHtons", (uint16_t))(48731);
            fixed.addr = F(uint32_t, "sceNetHtonl", (uint32_t))(0x7f000001);
            int bound = F(int, "sceNetBind", (int, const void*, int))(s2, &fixed, sizeof fixed);
            if (bound == 0) {
                const char msg[] = "jstars-selftest";
                int sent = F(int, "sceNetSendto", (int, const void*, size_t, int, const void*, int))(s, msg, sizeof msg, 0, &fixed, sizeof fixed);
                CHECK(sent == (int)sizeof msg, "net: a datagram is sent to the peer socket");
                int got = -1;
                for (int tries = 0; tries < 200 && got < 0; ++tries) {
                    int fl = sizeof from;
                    got = F(int, "sceNetRecvfrom", (int, void*, size_t, int, void*, int*))(s2, buf, sizeof buf, 0x80, &from, &fl);
                    if (got < 0) rt_sleep_ms_for_selftest(1);
                }
                CHECK(got == (int)sizeof msg && !memcmp(buf, msg, sizeof msg), "net: the peer socket receives the same datagram");
                CHECK(from.family == 2 && from.addr == fixed.addr, "net: the sender address is translated back into the console layout");
            } else {
                rt_logf("selftest: UDP port 48731 could not be bound on this host; the loopback datagram check was skipped");
            }
            F(int, "sceNetSocketClose", (int))(s2);
        }
        (void)alen; (void)self_addr;
        CHECK(F(int, "sceNetSocketAbort", (int, int))(s, 0) == 0, "net: socketAbort is accepted for an open socket");
        CHECK(F(int, "sceNetSocketClose", (int))(s) == 0, "net: socket closes");
        CHECK(F(int, "sceNetSocketClose", (int))(s) == -1 && *err == 9, "net: closing the same socket twice reports EBADF");
        st = -1;
        CHECK(F(int, "sceNetCtlGetState", (int*))(&st) == 0 && (st == 3 || st == 0),
              "net: online NetCtl reports IP obtained (or disconnected when this host has no address)");
        rt_net_configure(0, 0);
    }

    (void)fsbad;
    rt_logf("selftest-host: %d checks, %d failed", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
