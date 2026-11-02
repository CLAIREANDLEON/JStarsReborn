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
static int np_cb_calls, np_cb_user_id, np_cb_state; static void* np_cb_userdata; static char np_cb_name[20];
static void GUEST_ABI np_state_cb(int user_id, uint32_t state, void* np_id, void* userdata) {
    ++np_cb_calls; np_cb_user_id = user_id; np_cb_state = (int)state; np_cb_userdata = userdata;
    if (np_id) memcpy(np_cb_name, np_id, sizeof np_cb_name);
}
// Matching2 context callback: runs on the library's own thread, so the fields are read after polling.
static volatile int m2_cb_calls, m2_cb_ctx, m2_cb_event, m2_cb_cause, m2_cb_error;
static void* volatile m2_cb_userdata;
static void GUEST_ABI m2_context_cb(uint16_t ctx_id, uint16_t event, uint8_t cause, int error_code, void* userdata) {
    m2_cb_ctx = ctx_id; m2_cb_event = event; m2_cb_cause = cause; m2_cb_error = error_code; m2_cb_userdata = userdata;
    __atomic_add_fetch(&m2_cb_calls, 1, __ATOMIC_SEQ_CST);
}
static int m2_wait_calls(int n) {   // up to 2 s
    for (int i = 0; i < 200 && __atomic_load_n(&m2_cb_calls, __ATOMIC_SEQ_CST) < n; ++i) rt_sleep_ms_for_selftest(10);
    return __atomic_load_n(&m2_cb_calls, __ATOMIC_SEQ_CST);
}
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
        _Static_assert(sizeof(struct GuestAddr) == 16, "SceNetSockaddrIn is 16 bytes");
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

        // P2P (virtual port) sockets: this game asks for socket type 6, where several logical channels share one
        // real UDP port and are told apart by a virtual port. Two channels are opened on the same port here and
        // each must receive only what was addressed to it.
        {
            const uint16_t REAL_PORT = 48737;
            int a1 = F(int, "sceNetSocket", (const char*, int, int, int))("p2p-a", 2, 6, 0);
            int b1 = F(int, "sceNetSocket", (const char*, int, int, int))("p2p-b", 2, 6, 0);
            CHECK(a1 >= 1 && b1 >= 1 && a1 != b1, "net: two P2P sockets are created");
            struct GuestAddr pa, pb;
            memset(&pa, 0, sizeof pa); memset(&pb, 0, sizeof pb);
            pa.len = pb.len = sizeof pa; pa.family = pb.family = 2;
            pa.port = pb.port = F(uint16_t, "sceNetHtons", (uint16_t))(REAL_PORT);
            pa.addr = pb.addr = F(uint32_t, "sceNetHtonl", (uint32_t))(0x7f000001);
            pa.vport = F(uint16_t, "sceNetHtons", (uint16_t))(100);
            pb.vport = F(uint16_t, "sceNetHtons", (uint16_t))(200);
            int ba = F(int, "sceNetBind", (int, const void*, int))(a1, &pa, sizeof pa);
            int bb = F(int, "sceNetBind", (int, const void*, int))(b1, &pb, sizeof pb);
            if (ba == 0 && bb == 0) {
                CHECK(1, "net: two P2P sockets share one real UDP port with different virtual ports");
                struct GuestAddr dup;
                memcpy(&dup, &pa, sizeof dup);
                int c1 = F(int, "sceNetSocket", (const char*, int, int, int))("p2p-dup", 2, 6, 0);
                CHECK(F(int, "sceNetBind", (int, const void*, int))(c1, &dup, sizeof dup) == -1 && *err == 48,
                      "net: binding a virtual port that is already in use reports EADDRINUSE");
                F(int, "sceNetSocketClose", (int))(c1);

                const char to_b[] = "for-virtual-port-200";
                CHECK(F(int, "sceNetSendto", (int, const void*, size_t, int, const void*, int))(a1, to_b, sizeof to_b, 0, &pb, sizeof pb) == (int)sizeof to_b,
                      "net: a P2P datagram is sent to another virtual port on the same port");
                char got[64];
                struct GuestAddr src;
                int n = -1;
                for (int tries = 0; tries < 300 && n < 0; ++tries) {
                    int sl = sizeof src;
                    n = F(int, "sceNetRecvfrom", (int, void*, size_t, int, void*, int*))(b1, got, sizeof got, 0x80, &src, &sl);
                    if (n < 0) rt_sleep_ms_for_selftest(1);
                }
                CHECK(n == (int)sizeof to_b && !memcmp(got, to_b, sizeof to_b), "net: the addressed virtual port receives the datagram");
                CHECK(src.vport == pa.vport, "net: the sender's virtual port is reported back in the address");
                CHECK(F(int, "sceNetRecvfrom", (int, void*, size_t, int, void*, int*))(a1, got, sizeof got, 0x80, NULL, NULL) == -1 && *err == 35,
                      "net: the other virtual port on the same UDP port received nothing");
            } else {
                rt_logf("selftest: UDP port %u could not be bound on this host; the virtual-port checks were skipped", (unsigned)REAL_PORT);
            }
            // Leave one datagram unread on a1, then close both: the shared UDP port must really be given back.
            if (ba == 0 && bb == 0) {
                const char to_a[] = "left-unread";
                F(int, "sceNetSendto", (int, const void*, size_t, int, const void*, int))(b1, to_a, sizeof to_a, 0, &pa, sizeof pa);
                rt_sleep_ms_for_selftest(20);
            }
            F(int, "sceNetSocketClose", (int))(a1);
            F(int, "sceNetSocketClose", (int))(b1);
            if (ba == 0 && bb == 0) {
                int plain = F(int, "sceNetSocket", (const char*, int, int, int))("after-p2p", 2, 2, 0);
                struct GuestAddr pp;
                memcpy(&pp, &pa, sizeof pp);
                pp.vport = 0;
                CHECK(plain >= 1 && F(int, "sceNetBind", (int, const void*, int))(plain, &pp, sizeof pp) == 0,
                      "net: closing the last P2P socket on a UDP port releases the real port");
                F(int, "sceNetSocketClose", (int))(plain);
            }
        }
        rt_net_configure(0, 0);
    }

    // ---- NP sign-in state callback -----------------------------------------------------------------------
    // The console delivers the state from inside sceNpCheckCallback, on the game's own thread. These checks
    // use a real guest-ABI callback, so they also cover the call across the boundary.
    {
        int marker = 0;
        CHECK(F(int, "sceNpRegisterStateCallback", (void*, void*))(NULL, &marker) == (int)0x80550003,
              "np: registering a NULL state callback is an INVALID_ARGUMENT");
        rt_net_configure(1, 0);
        CHECK(F(int, "sceNpRegisterStateCallback", (void*, void*))((void*)np_state_cb, &marker) == 0,
              "np: a state callback is registered");
        CHECK(F(int, "sceNpRegisterStateCallback", (void*, void*))((void*)np_state_cb, &marker) == (int)0x80550008,
              "np: registering a second state callback reports ALREADY_REGISTERED");
        np_cb_calls = 0;
        CHECK(F(int, "sceNpCheckCallback", (void))() == 0 && np_cb_calls == 1,
              "np: sceNpCheckCallback delivers the pending state once");
        CHECK(np_cb_state == 2 && np_cb_user_id == 1 && np_cb_userdata == &marker,
              "np: the callback receives SIGNED_IN, the local user id and its own userdata");
        CHECK(np_cb_name[0] != 0, "np: the callback receives an NP id carrying the local online id");
        CHECK(F(int, "sceNpCheckCallback", (void))() == 0 && np_cb_calls == 1,
              "np: a second sceNpCheckCallback does not repeat an unchanged state");
        int st2 = -1;
        CHECK(F(int, "sceNpGetState", (int, int*))(1, &st2) == 0 && st2 == 2, "np: online, sceNpGetState reports SIGNED_IN");
        char online_id[20];
        memset(online_id, 0, sizeof online_id);
        CHECK(F(int, "sceNpGetOnlineId", (int, void*))(1, online_id) == 0 && online_id[0] != 0 && online_id[16] == 0,
              "np: the online id is returned, 16 characters and terminated");
        CHECK(F(int, "sceNpGetOnlineId", (int, void*))(7, online_id) == (int)0x80550007, "np: an unknown user reports USER_NOT_FOUND");
        CHECK(F(int, "sceNpUnregisterStateCallback", (void))() == 0, "np: the state callback is unregistered");
        rt_net_configure(0, 0);
        CHECK(F(int, "sceNpGetState", (int, int*))(1, &st2) == 0 && st2 == 1, "np: offline, sceNpGetState reports SIGNED_OUT");
        CHECK(F(int, "sceNpGetOnlineId", (int, void*))(1, online_id) == (int)0x80550006, "np: offline, the online id reports SIGNED_OUT");
    }

    // ---- NP Matching2 contexts: lifetime and the asynchronous start report -----------------------------------
    // Error values follow shadPS4's np_error.h (a reference, not the console). Nothing here is a room or a
    // server: the checks cover only what the context functions themselves promise.
    {
        typedef int (GUEST_ABI * CreateFn)(const void*, uint16_t*);
        typedef int (GUEST_ABI * IdFn)(uint16_t);
        CreateFn create = (CreateFn)rt_find_host("sceNpMatching2CreateContext");
        int (GUEST_ABI * start)(uint16_t, uint64_t) = (int (GUEST_ABI*)(uint16_t, uint64_t))rt_find_host("sceNpMatching2ContextStart");
        IdFn stop = (IdFn)rt_find_host("sceNpMatching2ContextStop");
        IdFn abort_start = (IdFn)rt_find_host("sceNpMatching2AbortContextStart");
        IdFn destroy = (IdFn)rt_find_host("sceNpMatching2DestroyContext");
        int (GUEST_ABI * server)(uint16_t, uint16_t*) = (int (GUEST_ABI*)(uint16_t, uint16_t*))rt_find_host("sceNpMatching2GetServerId");
        int (GUEST_ABI * reg_room)(uint16_t, void*, void*) = (int (GUEST_ABI*)(uint16_t, void*, void*))rt_find_host("sceNpMatching2RegisterRoomEventCallback");
        CHECK(create && start && stop && abort_start && destroy && server && reg_room, "m2: the context functions are exported");
        uint8_t param[0x28];
        memset(param, 0, sizeof param);
        const uint32_t label = 0;
        memcpy(param + 24, &label, 4);
        const uint64_t psize = sizeof param;
        memcpy(param + 32, &psize, 8);
        uint16_t ctx = 0, ctx2 = 0, sid = 0;
        F(int, "sceNpMatching2Terminate", (void))();
        CHECK(create(param, &ctx) == (int)0x80550C03, "m2: CreateContext before Initialize reports NOT_INITIALIZED");
        CHECK(F(int, "sceNpMatching2Initialize", (const void*))(NULL) == 0, "m2: Initialize");
        CHECK(create(NULL, &ctx) == (int)0x80550C0A && create(param, NULL) == (int)0x80550C0A,
              "m2: CreateContext with a NULL parameter or id pointer reports INVALID_ARGUMENT");
        CHECK(create(param, &ctx) == 0 && ctx >= 1, "m2: CreateContext returns a context id from 1");
        CHECK(create(param, &ctx2) == 0 && ctx2 != ctx, "m2: a second context gets a different id");
        CHECK(server(ctx, &sid) == 0 && sid == 1, "m2: GetServerId reports the one local server");
        CHECK(server(999, &sid) == (int)0x80550C0B, "m2: an unknown context id reports INVALID_CONTEXT_ID");
        CHECK(reg_room(ctx, (void*)m2_context_cb, NULL) == 0 && reg_room(77, NULL, NULL) == (int)0x80550C0B,
              "m2: a room event callback is accepted for a real context only");
        CHECK(stop(ctx) == (int)0x80550C08, "m2: stopping a context that never started reports CONTEXT_NOT_STARTED");
        CHECK(abort_start(ctx) == (int)0x80550C08, "m2: aborting a start that is not pending reports CONTEXT_NOT_STARTED");
        int marker = 0;
        CHECK(F(int, "sceNpMatching2RegisterContextCallback", (void*, void*))((void*)m2_context_cb, &marker) == 0,
              "m2: the context callback is registered");
        m2_cb_calls = 0;
        CHECK(start(ctx, 10 * 1000 * 1000) == 0, "m2: ContextStart is accepted");
        CHECK(m2_cb_calls == 0, "m2: the start is reported asynchronously, not inside ContextStart");
        CHECK(start(ctx, 0) == (int)0x80550C07, "m2: a second ContextStart while one is pending reports CONTEXT_ALREADY_STARTED");
        CHECK(m2_wait_calls(1) == 1, "m2: the start report arrives once");
        CHECK(m2_cb_ctx == ctx && m2_cb_event == 0x6F02 && m2_cb_cause == 11 && m2_cb_error == 0 && m2_cb_userdata == &marker,
              "m2: the report is CONTEXT_STARTED, cause CONTEXT_ACTION, no error, with the game's userdata");
        CHECK(start(ctx, 0) == (int)0x80550C07, "m2: starting a started context reports CONTEXT_ALREADY_STARTED");
        CHECK(stop(ctx) == 0 && m2_cb_calls == 2 && m2_cb_event == 0x6F03 && m2_cb_error == 0,
              "m2: ContextStop reports CONTEXT_STOPPED");
        CHECK(stop(ctx) == (int)0x80550C08, "m2: stopping it again reports CONTEXT_NOT_STARTED");
        // Abort while the start is pending: the report is STOPPED with ABORTED, and the context is not started.
        m2_cb_calls = 0;
        CHECK(start(ctx2, 0) == 0 && abort_start(ctx2) == 0, "m2: a pending start can be aborted");
        CHECK(m2_wait_calls(1) == 1 && m2_cb_ctx == ctx2 && m2_cb_event == 0x6F03 && m2_cb_error == (int)0x80550C2A,
              "m2: an aborted start reports CONTEXT_STOPPED with ABORTED");
        CHECK(stop(ctx2) == (int)0x80550C08, "m2: an aborted context is not started");
        // Destroy while the start is pending: nothing may be reported for the dead context.
        m2_cb_calls = 0;
        CHECK(start(ctx2, 0) == 0 && destroy(ctx2) == 0, "m2: a context can be destroyed while its start is pending");
        uint16_t ctx3 = 0;
        CHECK(create(param, &ctx3) == 0 && ctx3 == ctx2, "m2: the freed slot is reused at once");
        rt_sleep_ms_for_selftest(150);
        CHECK(m2_cb_calls == 0, "m2: the destroyed context's start is never reported, even though its id was reused");
        CHECK(stop(ctx3) == (int)0x80550C08, "m2: the new context in the reused slot is not started");
        CHECK(destroy(ctx3) == 0 && destroy(ctx3) == (int)0x80550C0B, "m2: destroying twice reports INVALID_CONTEXT_ID");
        uint16_t more[4]; int made = 0, rc = 0;
        for (int i = 0; i < 4; ++i) { rc = create(param, &more[i]); if (rc == 0) ++made; else break; }
        CHECK(made == 3 && rc == (int)0x80550C04, "m2: the fifth context reports CONTEXT_MAX");
        for (int i = 0; i < made; ++i) destroy(more[i]);
        CHECK(destroy(ctx) == 0, "m2: the first context is destroyed");
        CHECK(F(int, "sceNpMatching2Terminate", (void))() == 0 && server(ctx, &sid) == (int)0x80550C03,
              "m2: after Terminate the library reports NOT_INITIALIZED");
    }

    // ---- NP requests: the gate the game hits when it opens Online Battle -----------------------------------
    {
        rt_net_configure(1, 0);
        struct { uint64_t size; uint64_t affinity; int priority; int pad; } ap = {24, 0, 0, 0};
        CHECK(F(int, "sceNpCreateAsyncRequest", (const void*))(NULL) == (int)0x80550003, "np: an async request with no parameter is INVALID_ARGUMENT");
        ap.size = 8;
        CHECK(F(int, "sceNpCreateAsyncRequest", (const void*))(&ap) == (int)0x80550004, "np: an async request with the wrong parameter size is INVALID_SIZE");
        ap.size = 24;
        int req = F(int, "sceNpCreateAsyncRequest", (const void*))(&ap);
        CHECK(req > 0x20000000, "np: an async request is created with a console-shaped id");
        int res = -1;
        char oid[20];
        memset(oid, 0, sizeof oid);
        F(int, "sceNpGetOnlineId", (int, void*))(1, oid);
        CHECK(F(int, "sceNpCheckNpAvailability", (int, const void*))(req, oid) == 0, "np: the availability check is accepted");
        CHECK(F(int, "sceNpPollAsync", (int, int*))(req, &res) == 0 && res == 0, "np: polling reports the check finished, network available");
        CHECK(F(int, "sceNpPollAsync", (int, int*))(req, NULL) == (int)0x80550003, "np: polling with no result pointer is INVALID_ARGUMENT");
        CHECK(F(int, "sceNpPollAsync", (int, int*))(0x20000000 + 99, &res) == (int)0x8055000B, "np: polling an unknown request reports REQUEST_NOT_FOUND");
        struct { uint64_t size; int user; int pad; uint64_t features; uint8_t reserved[32]; } pp;
        memset(&pp, 0, sizeof pp);
        pp.size = sizeof pp; pp.user = 1; pp.features = 1;
        uint8_t plus_result[33];
        memset(plus_result, 0xAA, sizeof plus_result);
        CHECK(sizeof pp == 56, "np: the PS Plus parameter is the console's 56 bytes");
        CHECK(F(int, "sceNpCheckPlus", (int, const void*, void*))(req, &pp, plus_result) == 0 && plus_result[0] == 1,
              "np: the PS Plus check reports the local user as authorized");
        pp.features = 9;
        CHECK(F(int, "sceNpCheckPlus", (int, const void*, void*))(req, &pp, plus_result) == (int)0x80550003,
              "np: an unknown PS Plus feature is INVALID_ARGUMENT");
        CHECK(F(int, "sceNpAbortRequest", (int))(req) == 0, "np: a request can be aborted");
        CHECK(F(int, "sceNpPollAsync", (int, int*))(req, &res) == 0 && res == (int)0x8055000C, "np: an aborted request reports ABORTED");
        CHECK(F(int, "sceNpDeleteRequest", (int))(req) == 0, "np: a request is deleted");
        CHECK(F(int, "sceNpDeleteRequest", (int))(req) == (int)0x8055000B, "np: deleting it twice reports REQUEST_NOT_FOUND");
        char a_id[36], b_id[36];
        memset(a_id, 0, sizeof a_id); memset(b_id, 0, sizeof b_id);
        memcpy(a_id, "player-one", 10); memcpy(b_id, "player-one", 10);
        CHECK(F(int, "sceNpCmpNpId", (const void*, const void*))(a_id, b_id) == 0, "np: two equal NP ids compare equal");
        memcpy(b_id, "player-two", 10);
        CHECK(F(int, "sceNpCmpNpId", (const void*, const void*))(a_id, b_id) != 0, "np: two different NP ids do not compare equal");
        // Both take an online id, and the language one also takes a request id. Passing a user id here was the
        // mistake that kept the game out of Online Battle, so the shapes are pinned by these checks.
        char self_id[20];
        memset(self_id, 0, sizeof self_id);
        F(int, "sceNpGetOnlineId", (int, void*))(1, self_id);
        char country[4];
        memset(country, 0xAA, sizeof country);
        CHECK(F(int, "sceNpGetAccountCountry", (const void*, void*))(self_id, country) == 0 && country[0] && country[2] == 0,
              "np: an account country is reported as a two-character code");
        CHECK(F(int, "sceNpGetAccountCountry", (const void*, void*))(NULL, country) == (int)0x80550003, "np: a country query with no online id is INVALID_ARGUMENT");
        char lang[16];
        memset(lang, 0xAA, sizeof lang);
        int lreq = F(int, "sceNpCreateAsyncRequest", (const void*))(&ap);
        CHECK(F(int, "sceNpGetAccountLanguage", (int, const void*, void*))(lreq, self_id, lang) == 0 && lang[0] == 'e' && lang[1] == 'n' && lang[2] == 0,
              "np: an account language is reported as a language-code string on a request");
        int lres = -1;
        CHECK(F(int, "sceNpPollAsync", (int, int*))(lreq, &lres) == 0 && lres == 0, "np: the language request finished successfully");
        CHECK(F(int, "sceNpGetAccountLanguage", (int, const void*, void*))(0x20000000 + 98, self_id, lang) == (int)0x8055000B,
              "np: a language query on an unknown request reports REQUEST_NOT_FOUND");
        F(int, "sceNpDeleteRequest", (int))(lreq);

        // ---- WebApi: local contexts, and a send that honestly fails -------------------------------------
        {
            char wid[20];
            memset(wid, 0, sizeof wid);
            memcpy(wid, "Player", 6);
            int lib = F(int, "sceNpWebApiInitialize", (int, uint64_t))(1, 65536);
            CHECK(lib >= 1, "webapi: a library context is created");
            CHECK(F(int, "sceNpWebApiCreateContext", (int, const void*))(lib, NULL) == (int)0x80552902,
                  "webapi: a context with no online id is INVALID_ARGUMENT");
            int uc = F(int, "sceNpWebApiCreateContext", (int, const void*))(lib, wid);
            CHECK(uc >= 1, "webapi: a user context is created");
            CHECK(F(int, "sceNpWebApiCreateContext", (int, const void*))(lib, wid) == (int)0x8055290A,
                  "webapi: the same user context twice reports ALREADY_EXIST");
            int64_t rid = 0;
            CHECK(F(int, "sceNpWebApiCreateRequest", (int, const char*, const char*, int, const void*, int64_t*))(uc, "sdk:userProfile", "/v1/users/me", 0, NULL, &rid) == 0 && rid > 0,
                  "webapi: a request is created");
            CHECK(F(int, "sceNpWebApiSendRequest", (int64_t, const void*, uint64_t, void*))(rid, NULL, 0, NULL) == (int)0x80552907,
                  "webapi: a request with no local answer reports NOT_SIGNED_IN, because this machine has no PlayStation Network session");
            CHECK(F(int, "sceNpWebApiGetErrorCode", (void))() == (int)0x80552907, "webapi: the last error is readable");
            int http = -1;
            CHECK(F(int, "sceNpWebApiGetHttpStatusCode", (int64_t, int*))(rid, &http) == 0 && http == 0, "webapi: no HTTP status, because no response arrived");
            CHECK(F(int, "sceNpWebApiReadData", (int64_t, void*, uint64_t))(rid, wid, 4) == 0, "webapi: there is no response data to read");

            // The block list is the one request with a truthful local answer: nobody is blocked here.
            int64_t bid = 0;
            CHECK(F(int, "sceNpWebApiCreateRequest", (int, const char*, const char*, int, const void*, int64_t*))(uc, "userProfile", "/v1/users/Player/blockList", 0, NULL, &bid) == 0 && bid > 0,
                  "webapi: a block-list request is created");
            CHECK(F(int, "sceNpWebApiSendRequest", (int64_t, const void*, uint64_t, void*))(bid, NULL, 0, NULL) == 0,
                  "webapi: the block-list request is answered on this machine");
            CHECK(F(int, "sceNpWebApiGetHttpStatusCode", (int64_t, int*))(bid, &http) == 0 && http == 200, "webapi: the local answer carries HTTP 200");
            char body[128];
            memset(body, 0, sizeof body);
            int got_total = 0, chunk;
            while ((chunk = F(int, "sceNpWebApiReadData", (int64_t, void*, uint64_t))(bid, body + got_total, 8)) > 0) got_total += chunk;
            CHECK(chunk == 0 && got_total > 0 && strstr(body, "\"blockList\":[]") != NULL,
                  "webapi: the body reads back in pieces and ends with 0, and the block list is empty");
            F(int, "sceNpWebApiDeleteRequest", (int64_t))(bid);
            CHECK(F(int, "sceNpWebApiDeleteRequest", (int64_t))(rid) == 0, "webapi: a request is deleted");
            CHECK(F(int, "sceNpWebApiDeleteRequest", (int64_t))(rid) == (int)0x80552906, "webapi: deleting it twice reports REQUEST_NOT_FOUND");
            char parsed[36];
            CHECK(F(int, "sceNpWebApiUtilityParseNpId", (const char*, void*))("{\"onlineId\":\"SomePlayer\"}", parsed) == 0 && !strcmp(parsed, "SomePlayer"),
                  "webapi: an online id is parsed out of a JSON object");
            CHECK(F(int, "sceNpWebApiUtilityParseNpId", (const char*, void*))("{\"other\":1}", parsed) == (int)0x80552902,
                  "webapi: JSON without an online id is INVALID_ARGUMENT");
            CHECK(F(int, "sceNpWebApiDeleteContext", (int))(uc) == 0, "webapi: a user context is deleted");
            CHECK(F(int, "sceNpWebApiDeleteContext", (int))(uc) == (int)0x80552905, "webapi: deleting it twice reports USER_CONTEXT_NOT_FOUND");
        }

        // ---- Score: a local board -----------------------------------------------------------------------
        {
            char sid[36];
            memset(sid, 0, sizeof sid);
            memcpy(sid, "Player", 6);
            CHECK(F(int, "sceNpScoreCreateNpTitleCtx", (uint32_t, const void*))(0xFFFFFFFFu, sid) == (int)0x80550704,
                  "score: an invalid service label is INVALID_ARGUMENT");
            CHECK(F(int, "sceNpScoreCreateNpTitleCtx", (uint32_t, const void*))(0, NULL) == (int)0x8055070C,
                  "score: a missing player id is INSUFFICIENT_ARGUMENT");
            int ctx = F(int, "sceNpScoreCreateNpTitleCtx", (uint32_t, const void*))(0, sid);
            CHECK(ctx >= 1, "score: a title context is created");
            int sreq = F(int, "sceNpScoreCreateRequest", (int))(ctx);
            CHECK(sreq >= 1, "score: a request is created");
            CHECK(F(int, "sceNpScoreCreateRequest", (int))(99) == (int)0x8055070E, "score: a request on an unknown context is INVALID_ID");
            uint32_t tmp_rank = 0;
            CHECK(F(int, "sceNpScoreRecordScoreAsync", (int, uint32_t, int64_t, const void*, const void*, uint32_t*, const void*, void*))
                      (sreq, 1, 1234, NULL, NULL, &tmp_rank, NULL, NULL) == 0 && tmp_rank == 1,
                  "score: a score is recorded locally and ranks first on an empty board");
            int sres = -1;
            CHECK(F(int, "sceNpScorePollAsync", (int, int*))(sreq, &sres) == 0 && sres == 0, "score: the request reports success");
            uint32_t total = 99;
            char rank_buf[64];
            memset(rank_buf, 0xAA, sizeof rank_buf);
            CHECK(F(int, "sceNpScoreGetRankingByRangeAsync", (int, uint32_t, uint32_t, void*, uint64_t, void*, uint64_t, void*, uint64_t, uint64_t, void*, uint32_t*, void*))
                      (sreq, 1, 1, rank_buf, sizeof rank_buf, NULL, 0, NULL, 0, 1, NULL, &total, NULL) == 0 && total == 0 && rank_buf[0] == 0,
                  "score: a ranking query reports an empty board and clears the caller's array");
            CHECK(F(int, "sceNpScoreDeleteRequest", (int))(sreq) == 0, "score: a request is deleted");
            CHECK(F(int, "sceNpScoreDeleteNpTitleCtx", (int))(ctx) == 0, "score: a title context is deleted");
            CHECK(F(int, "sceNpScoreDeleteNpTitleCtx", (int))(ctx) == (int)0x8055070E, "score: deleting it twice is INVALID_ID");
        }
        rt_net_configure(0, 0);
    }

    (void)fsbad;
    rt_logf("selftest-host: %d checks, %d failed", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
