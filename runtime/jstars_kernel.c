// libkernel services for the J-Stars runtime: process parameters, TLS, threads and
// synchronization, memory, time. Each function implements the console contract as the game's own
// code uses it. Where the host cannot honor a hint (priority, affinity) the value is recorded and
// returned by the matching getter, and the log says so; nothing claims behaviour beyond that.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;
#define SCE_ERR(e) ((int)(0x80020000u | (unsigned)(e)))
#define SCE_KERNEL_ERROR_EINVAL SCE_ERR(22)

static void* GUEST_ABI k_sceKernelGetProcParam(void) { return rt_procparam(); }
static void* g_heap_api;
static int GUEST_ABI k_RtldSetApplicationHeapAPI(void* api) { g_heap_api = api; return 0; }

// ---- errno / stack guard / program name -----------------------------------------------------
static __thread int t_errno;
static int* GUEST_ABI k_error(void) { return &t_errno; }
static uint64_t g_stack_guard = 0x2f1b9c47d3a85e00ull; // value is arbitrary; only equality matters
static const char* g_progname = "eboot.bin";

// ---- TLS (ELF dynamic TLS for the bundled modules) --------------------------------------------
typedef struct { uint64_t module, offset; } TlsIndex;
#define MAX_TLS_MODULES 8
static __thread void* t_tls_block[MAX_TLS_MODULES];
static void* GUEST_ABI k_tls_get_addr(const TlsIndex* ti) {
    uint64_t m = ti->module;
    if (m >= MAX_TLS_MODULES) { rt_logf("__tls_get_addr: module id %" PRIu64 " out of range", m); abort(); }
    if (!t_tls_block[m]) {
        const void* init; size_t filesz, memsz;
        if (!rt_tls_template(m, &init, &filesz, &memsz)) { rt_logf("__tls_get_addr: unknown TLS module %" PRIu64, m); abort(); }
        void* b = os_alloc_aligned(64, (memsz + 63) & ~(size_t)63);
        memset(b, 0, memsz);
        memcpy(b, init, filesz);
        t_tls_block[m] = b;
    }
    return (char*)t_tls_block[m] + ti->offset;
}

// ---- threads ------------------------------------------------------------------------------------
typedef struct GuestThread {
    pthread_t host;
    void* (GUEST_ABI* entry)(void*);
    void* arg;
    char name[32];
    int detached;
    int prio;
    uint64_t affinity;
} GuestThread;
static __thread GuestThread* t_self;
// Live guest threads, for the diagnostic sampler (rt_sample_threads).
static GuestThread* g_thr[128]; static int g_nthr; static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;
static void thr_add(GuestThread* g) { pthread_mutex_lock(&g_thr_mu); if (g_nthr < 128) g_thr[g_nthr++] = g; pthread_mutex_unlock(&g_thr_mu); }
static void thr_del(GuestThread* g) {
    pthread_mutex_lock(&g_thr_mu);
    for (int i = 0; i < g_nthr; i++) if (g_thr[i] == g) { g_thr[i] = g_thr[--g_nthr]; break; }
    pthread_mutex_unlock(&g_thr_mu);
}
void rt_sample_threads(void) {
    typedef struct { char name[32]; OsThreadSample s; int ok; } Rec;
    static Rec recs[128];
    int n = 0;
    pthread_mutex_lock(&g_thr_mu);
    for (int i = 0; i < g_nthr && n < 128; i++) { // each thread is resumed before the next is suspended; nothing is logged meanwhile
        snprintf(recs[n].name, sizeof recs[n].name, "%s", g_thr[i]->name);
        recs[n].ok = os_thread_sample((uintptr_t)g_thr[i]->host, &recs[n].s) == 0;
        n++;
    }
    pthread_mutex_unlock(&g_thr_mu);
    rt_logf("thread sample: %d guest threads", n);
    for (int i = 0; i < n; i++) {
        char w[200], g[200], line[1400] = "";
        if (!recs[i].ok) { rt_logf("  [%s] (could not be sampled)", recs[i].name); continue; }
        const char* where = rt_locate(recs[i].s.rip, w, sizeof w);
        if (!strncmp(where, "host/other", 10)) where = os_where(recs[i].s.rip, w, sizeof w);
        int shown = 0;
        for (int k = 0; k < recs[i].s.nwords && shown < 14; k++) {
            const char* l = rt_locate(recs[i].s.words[k], g, sizeof g);
            if (strncmp(l, "host/other", 10)) { size_t o = strlen(line); snprintf(line + o, sizeof line - o, "%s%s", shown ? " <- " : "", l); shown++; }
        }
        rt_logf("  [%s] rip=%s guest return addresses on stack: %s", recs[i].name, where, shown ? line : "(none)");
    }
}
void rt_thread_attach_main(void) {
    GuestThread* g = calloc(1, sizeof *g);
    g->host = pthread_self(); g->affinity = ~0ull;
    snprintf(g->name, sizeof g->name, "main");
    t_self = g;
    thr_add(g);
}
#define GUEST_OBJ_PAD 256 /* guest-private bytes in front of host objects the guest may poke at: see guest_obj_new */
typedef struct { size_t stack; int detach; int prio; uint64_t affinity; } GuestAttr;
static int GUEST_ABI k_attr_init(GuestAttr** a) { *a = calloc(1, GUEST_OBJ_PAD > sizeof **a ? GUEST_OBJ_PAD : sizeof **a); (*a)->stack = 1u << 20; (*a)->affinity = ~0ull; return 0; }
static int GUEST_ABI k_attr_destroy(GuestAttr** a) { free(*a); *a = NULL; return 0; }
static int GUEST_ABI k_attr_setstacksize(GuestAttr** a, size_t n) { if (!*a) return SCE_KERNEL_ERROR_EINVAL; (*a)->stack = n < 65536 ? 65536 : n; return 0; }
static int GUEST_ABI k_attr_setdetachstate(GuestAttr** a, int s) { if (!*a) return SCE_KERNEL_ERROR_EINVAL; (*a)->detach = s == 1; return 0; }
static int GUEST_ABI k_attr_setaffinity(GuestAttr** a, uint64_t m) { if (!*a) return SCE_KERNEL_ERROR_EINVAL; (*a)->affinity = m; return 0; }
// Accepted and ignored by this host: scheduling policy, inheritance, guard size, priority.
static int GUEST_ABI k_attr_ignore_int(GuestAttr** a, int v) { (void)v; return *a ? 0 : SCE_KERNEL_ERROR_EINVAL; }
static int GUEST_ABI k_attr_setschedparam(GuestAttr** a, const int* p) { if (!*a) return SCE_KERNEL_ERROR_EINVAL; (*a)->prio = *p; return 0; }
static int GUEST_ABI k_attr_setguardsize(GuestAttr** a, size_t n) { (void)n; return *a ? 0 : SCE_KERNEL_ERROR_EINVAL; }
static int GUEST_ABI k_attr_get(GuestThread* t, GuestAttr** a) { k_attr_init(a); (*a)->prio = t->prio; (*a)->affinity = t->affinity; return 0; }
static int GUEST_ABI k_attr_getaffinity(GuestAttr** a, uint64_t* m) { *m = (*a)->affinity; return 0; }

// scePthreadExit must leave guest frames that carry no unwind information, so it cannot use pthread_exit (which unwinds:
// forced unwinding on Linux, RtlUnwind on Windows -> STATUS_BAD_FUNCTION_TABLE). Each created thread arms a register
// snapshot in its start routine; exiting restores it and returns the value from there. Same effect as returning from the entry.
static __thread void* t_exit_buf[5];
static __thread void* t_exit_value;
static __thread int t_exit_armed;
static void* host_thread_start(void* p) {
    GuestThread* g = p;
    t_self = g;
    const uint16_t cw = 0x037F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
    rt_logf("thread '%s' started", g->name);
    thr_add(g);
    t_exit_armed = 1;
    if (__builtin_setjmp(t_exit_buf) == 0) t_exit_value = g->entry(g->arg);
    t_exit_armed = 0;
    thr_del(g);
    return t_exit_value;
}
static int GUEST_ABI k_pthread_create(GuestThread** out, GuestAttr** attr, void* (GUEST_ABI* entry)(void*), void* arg, const char* name) {
    GuestThread* g = calloc(1, sizeof *g);
    g->entry = entry; g->arg = arg; g->affinity = ~0ull;
    snprintf(g->name, sizeof g->name, "%s", name ? name : "unnamed");
    pthread_attr_t at;
    pthread_attr_init(&at);
    size_t stack = attr && *attr ? (*attr)->stack : 1u << 20;
    if (attr && *attr) { g->detached = (*attr)->detach; g->prio = (*attr)->prio; g->affinity = (*attr)->affinity; }
    pthread_attr_setstacksize(&at, stack);
    if (g->detached) pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&g->host, &at, host_thread_start, g);
    pthread_attr_destroy(&at);
    if (r) { free(g); return SCE_ERR(r); }
    *out = g;
    return 0;
}
// Starts a detached guest-visible thread running entry(arg) (used by runtime services that call back into guest code
// from a thread of their own, such as the AvPlayer event callback). 0 on success.
const char* rt_current_thread_name(void) { return t_self ? t_self->name : "(not a guest thread: a host, GPU or driver thread)"; }
int rt_spawn_guest_thread(void* (GUEST_ABI* entry)(void*), void* arg, const char* name) {
    GuestAttr* at = NULL; k_attr_init(&at); at->detach = 1; at->stack = 1u << 20;
    GuestThread* out = NULL;
    int r = k_pthread_create(&out, &at, entry, arg, name);
    k_attr_destroy(&at);
    return r;
}
// Hooks for the online libraries (gpu/jsnet/jsnet_glue.cpp): their threads call back into the game (Matching2/Signaling
// callbacks), so they are created as guest-visible threads like scePthreadCreate's (guest TCB/TLS, scePthreadExit support).
static int GUEST_ABI k_pthread_join(GuestThread* t, void** ret);
static void GUEST_ABI k_pthread_exit(void* v);
int runtime_thread_spawn(void** out, void* (GUEST_ABI* entry)(void*), void* arg, uint64_t stack, const char* name) {
    if (!out || !entry) return SCE_KERNEL_ERROR_EINVAL;
    GuestAttr* at = NULL;
    k_attr_init(&at);
    if (stack) at->stack = stack < 65536 ? 65536 : stack;
    GuestThread* t = NULL;
    const int r = k_pthread_create(&t, &at, entry, arg, name ? name : "net");
    k_attr_destroy(&at);
    if (!r) *out = t;
    return r;
}
int runtime_thread_join_spawned(void* thread, void** result) { return thread ? k_pthread_join(thread, result) : SCE_KERNEL_ERROR_EINVAL; }
void runtime_thread_exit(void* value) { k_pthread_exit(value); __builtin_unreachable(); }
static GuestThread* GUEST_ABI k_pthread_self(void) { return t_self; }
static int GUEST_ABI k_pthread_join(GuestThread* t, void** ret) { int r = pthread_join(t->host, ret); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_pthread_detach(GuestThread* t) { int r = pthread_detach(t->host); return r ? SCE_ERR(r) : 0; }
static void GUEST_ABI k_pthread_exit(void* v) {
    if (t_exit_armed) { t_exit_value = v; __builtin_longjmp(t_exit_buf, 1); }
    pthread_exit(v);   // the process' main thread: nothing to restore
}
static int GUEST_ABI k_pthread_equal(GuestThread* a, GuestThread* b) { return a == b; }
static int GUEST_ABI k_pthread_yield(void) { sched_yield(); return 0; }
static int GUEST_ABI k_setprio(GuestThread* t, int p) { t->prio = p; return 0; }
static int GUEST_ABI k_getprio(GuestThread* t, int* p) { *p = t->prio; return 0; }
static int GUEST_ABI k_setaffinity(GuestThread* t, uint64_t m) { t->affinity = m; return 0; }
static int GUEST_ABI k_getaffinity(GuestThread* t, uint64_t* m) { *m = t->affinity; return 0; }

// ---- mutex / cond / rwlock / once / keys: created lazily when the guest slot is still zero ----
#ifdef _WIN32
#define COND_CLOCK CLOCK_REALTIME /* winpthreads waits on absolute realtime */
#else
#define COND_CLOCK CLOCK_MONOTONIC
#endif
static pthread_mutex_t g_lazy = PTHREAD_MUTEX_INITIALIZER;
// The game's own libc.prx reads and writes INSIDE the object a mutex/cond/rwlock slot points to: right after
// scePthreadMutexInit it does "or dword [obj+0x20], 0x100" (libc.prx+0x23a20, caught by the heap-guard build on
// Windows; it was a 4-byte overflow of the 8-byte block holding a winpthreads mutex, which corrupted the Windows heap
// and crashed free() in about 2% of starts). On the console these are libkernel-internal structures of a few hundred
// bytes, so the object a slot points to is a zeroed block with GUEST_OBJ_PAD bytes of guest-private space in front of
// the host primitive. The guest may scribble in that space; the host never reads it.
static void* guest_obj_new(size_t host_size) { return calloc(1, GUEST_OBJ_PAD + host_size); }
static inline void* guest_obj_host(void* blk) { return (char*)blk + GUEST_OBJ_PAD; }
typedef struct { int type; } MutexAttr;
static int GUEST_ABI k_mutexattr_init(MutexAttr** a) { *a = calloc(1, GUEST_OBJ_PAD); (*a)->type = 1; return 0; }
static int GUEST_ABI k_mutexattr_destroy(MutexAttr** a) { free(*a); *a = NULL; return 0; }
static int GUEST_ABI k_mutexattr_settype(MutexAttr** a, int t) { if (!*a || t < 1 || t > 4) return SCE_KERNEL_ERROR_EINVAL; (*a)->type = t; return 0; }
static int GUEST_ABI k_mutexattr_setprotocol(MutexAttr** a, int p) { (void)p; return *a ? 0 : SCE_KERNEL_ERROR_EINVAL; }
static pthread_mutex_t* mutex_of(void** slot, int type) {
    pthread_mutex_lock(&g_lazy);
    if (!*slot) {
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        // SCE types: 1 errorcheck, 2 recursive, 3 normal, 4 adaptive
        pthread_mutexattr_settype(&at, type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK);
        void* blk = guest_obj_new(sizeof(pthread_mutex_t));
        pthread_mutex_init(guest_obj_host(blk), &at);
        pthread_mutexattr_destroy(&at);
        *slot = blk;
    }
    pthread_mutex_t* m = guest_obj_host(*slot);
    pthread_mutex_unlock(&g_lazy);
    return m;
}
static int GUEST_ABI k_mutex_init(void** slot, MutexAttr** a, const char* name) { (void)name; *slot = NULL; mutex_of(slot, a && *a ? (*a)->type : 1); return 0; }
static int GUEST_ABI k_mutex_destroy(void** slot) { if (*slot) { pthread_mutex_destroy(guest_obj_host(*slot)); free(*slot); *slot = NULL; } return 0; }
static int GUEST_ABI k_mutex_lock(void** slot) { int r = pthread_mutex_lock(mutex_of(slot, 1)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_mutex_trylock(void** slot) { int r = pthread_mutex_trylock(mutex_of(slot, 1)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_mutex_unlock(void** slot) { int r = pthread_mutex_unlock(mutex_of(slot, 1)); return r ? SCE_ERR(r) : 0; }

static pthread_cond_t* cond_of(void** slot) {
    pthread_mutex_lock(&g_lazy);
    if (!*slot) {
        void* blk = guest_obj_new(sizeof(pthread_cond_t));
        pthread_condattr_t at; pthread_condattr_init(&at); pthread_condattr_setclock(&at, COND_CLOCK);
        pthread_cond_init(guest_obj_host(blk), &at); pthread_condattr_destroy(&at);
        *slot = blk;
    }
    pthread_cond_t* c = guest_obj_host(*slot);
    pthread_mutex_unlock(&g_lazy);
    return c;
}
static int GUEST_ABI k_cond_init(void** slot, void* attr, const char* name) { (void)attr; (void)name; *slot = NULL; cond_of(slot); return 0; }
static int GUEST_ABI k_cond_destroy(void** slot) { if (*slot) { pthread_cond_destroy(guest_obj_host(*slot)); free(*slot); *slot = NULL; } return 0; }
static int GUEST_ABI k_cond_wait(void** c, void** m) { int r = pthread_cond_wait(cond_of(c), mutex_of(m, 1)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_cond_timedwait(void** c, void** m, uint32_t usec) {
    struct timespec t; clock_gettime(COND_CLOCK, &t);
    t.tv_sec += usec / 1000000; t.tv_nsec += (long)(usec % 1000000) * 1000;
    if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    int r = pthread_cond_timedwait(cond_of(c), mutex_of(m, 1), &t);
    return r ? SCE_ERR(r == ETIMEDOUT ? 60 : r) : 0; // SCE_KERNEL_ERROR_ETIMEDOUT = 0x8002003c
}
static int GUEST_ABI k_cond_signal(void** c) { return pthread_cond_signal(cond_of(c)) ? SCE_KERNEL_ERROR_EINVAL : 0; }
static int GUEST_ABI k_cond_broadcast(void** c) { return pthread_cond_broadcast(cond_of(c)) ? SCE_KERNEL_ERROR_EINVAL : 0; }

static pthread_rwlock_t* rw_of(void** slot) {
    pthread_mutex_lock(&g_lazy);
    if (!*slot) { void* blk = guest_obj_new(sizeof(pthread_rwlock_t)); pthread_rwlock_init(guest_obj_host(blk), NULL); *slot = blk; }
    pthread_rwlock_t* r = guest_obj_host(*slot);
    pthread_mutex_unlock(&g_lazy);
    return r;
}
static int GUEST_ABI k_rw_init(void** s, void* a, const char* n) { (void)a; (void)n; *s = NULL; rw_of(s); return 0; }
static int GUEST_ABI k_rw_destroy(void** s) { if (*s) { pthread_rwlock_destroy(guest_obj_host(*s)); free(*s); *s = NULL; } return 0; }
static int GUEST_ABI k_rw_rdlock(void** s) { int r = pthread_rwlock_rdlock(rw_of(s)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_rw_wrlock(void** s) { int r = pthread_rwlock_wrlock(rw_of(s)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_rw_tryrdlock(void** s) { int r = pthread_rwlock_tryrdlock(rw_of(s)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_rw_trywrlock(void** s) { int r = pthread_rwlock_trywrlock(rw_of(s)); return r ? SCE_ERR(r) : 0; }
static int GUEST_ABI k_rw_unlock(void** s) { int r = pthread_rwlock_unlock(rw_of(s)); return r ? SCE_ERR(r) : 0; }

static int GUEST_ABI k_once(int* control, void (GUEST_ABI* fn)(void)) {
    pthread_mutex_lock(&g_lazy);
    int run = !*control;
    if (run) *control = 1;
    pthread_mutex_unlock(&g_lazy);
    if (run) fn();
    return 0;
}
static pthread_key_t host_key[256];
static int host_key_used[256];
static int GUEST_ABI k_key_create(uint32_t* key, void (GUEST_ABI* dtor)(void*)) {
    (void)dtor; // key destructors are not run at thread exit yet
    pthread_mutex_lock(&g_lazy);
    for (uint32_t i = 0; i < 256; ++i)
        if (!host_key_used[i]) { host_key_used[i] = 1; pthread_key_create(&host_key[i], NULL); *key = i; pthread_mutex_unlock(&g_lazy); return 0; }
    pthread_mutex_unlock(&g_lazy);
    return SCE_ERR(11);
}
static int GUEST_ABI k_key_delete(uint32_t key) { if (key >= 256 || !host_key_used[key]) return SCE_KERNEL_ERROR_EINVAL; pthread_key_delete(host_key[key]); host_key_used[key] = 0; return 0; }
static int GUEST_ABI k_setspecific(uint32_t key, const void* v) { if (key >= 256 || !host_key_used[key]) return SCE_KERNEL_ERROR_EINVAL; return pthread_setspecific(host_key[key], v) ? SCE_KERNEL_ERROR_EINVAL : 0; }
static void* GUEST_ABI k_getspecific(uint32_t key) { return key < 256 && host_key_used[key] ? pthread_getspecific(host_key[key]) : NULL; }

// ---- time -------------------------------------------------------------------------------------
static clockid_t clock_of(int id) { // FreeBSD clock ids -> host
    switch (id) {
    case 0: case 9: case 10: return CLOCK_REALTIME;
    case 4: case 5: case 7: case 8: case 11: case 12: return CLOCK_MONOTONIC;
    case 14: return CLOCK_THREAD_CPUTIME_ID;
    case 15: return CLOCK_PROCESS_CPUTIME_ID;
    default: return (clockid_t)-1;
    }
}
static int GUEST_ABI k_clock_gettime(int id, struct timespec* ts) {
    clockid_t c = clock_of(id);
    if (c == (clockid_t)-1) { t_errno = 22; return -1; }
    return clock_gettime(c, ts);
}
static int GUEST_ABI k_sceKernelClockGettime(int id, struct timespec* ts) { return k_clock_gettime(id, ts) ? SCE_KERNEL_ERROR_EINVAL : 0; }
static int GUEST_ABI k_gettimeofday(struct timeval* tv, void* tz) { return gettimeofday(tv, tz); }
static int GUEST_ABI k_sceKernelGettimeofday(struct timeval* tv) { return gettimeofday(tv, NULL) ? SCE_KERNEL_ERROR_EINVAL : 0; }
static int GUEST_ABI k_usleep(uint32_t us) { struct timespec t = {us / 1000000, (long)(us % 1000000) * 1000}; while (nanosleep(&t, &t) && errno == EINTR) {} return 0; }
static int GUEST_ABI k_nanosleep(const struct timespec* req, struct timespec* rem) { return nanosleep(req, rem); }
static struct timespec g_start;
static uint64_t GUEST_ABI k_getprocesstime(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)((t.tv_sec - g_start.tv_sec) * 1000000 + (t.tv_nsec - g_start.tv_nsec) / 1000); }
static uint64_t GUEST_ABI k_readtsc(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } // 1 GHz counter
static uint64_t GUEST_ABI k_tscfreq(void) { return 1000000000ull; }

// ---- exceptions and sched --------------------------------------------------------------------
static int GUEST_ABI k_sched_yield(void) { sched_yield(); return 0; }
static int GUEST_ABI k_getpid(void) { return 1; }
static int GUEST_ABI k_getpagesize(void) { return 16384; } // the console's page size

const HostSymbol kernel_symbols[] = {
    {"sceKernelGetProcParam", "libkernel", (void*)k_sceKernelGetProcParam, 0},
    {"_sceKernelRtldSetApplicationHeapAPI", "libkernel", (void*)k_RtldSetApplicationHeapAPI, 0},
    {"__error", "libkernel", (void*)k_error, 0},
    {"__stack_chk_guard", "libkernel", (void*)&g_stack_guard, 1},
    {"__progname", "libkernel", (void*)&g_progname, 1},
    {"__tls_get_addr", "libkernel", (void*)k_tls_get_addr, 0},
    {"scePthreadAttrInit", "libkernel", (void*)k_attr_init, 0},
    {"scePthreadAttrDestroy", "libkernel", (void*)k_attr_destroy, 0},
    {"scePthreadAttrSetstacksize", "libkernel", (void*)k_attr_setstacksize, 0},
    {"scePthreadAttrSetdetachstate", "libkernel", (void*)k_attr_setdetachstate, 0},
    {"scePthreadAttrSetaffinity", "libkernel", (void*)k_attr_setaffinity, 0},
    {"scePthreadAttrSetinheritsched", "libkernel", (void*)k_attr_ignore_int, 0},
    {"scePthreadAttrSetschedpolicy", "libkernel", (void*)k_attr_ignore_int, 0},
    {"scePthreadAttrSetschedparam", "libkernel", (void*)k_attr_setschedparam, 0},
    {"scePthreadAttrSetguardsize", "libkernel", (void*)k_attr_setguardsize, 0},
    {"scePthreadAttrGet", "libkernel", (void*)k_attr_get, 0},
    {"scePthreadAttrGetaffinity", "libkernel", (void*)k_attr_getaffinity, 0},
    {"scePthreadCreate", "libkernel", (void*)k_pthread_create, 0},
    {"scePthreadSelf", "libkernel", (void*)k_pthread_self, 0},
    {"scePthreadJoin", "libkernel", (void*)k_pthread_join, 0},
    {"scePthreadDetach", "libkernel", (void*)k_pthread_detach, 0},
    {"scePthreadExit", "libkernel", (void*)k_pthread_exit, 0},
    {"scePthreadEqual", "libkernel", (void*)k_pthread_equal, 0},
    {"scePthreadYield", "libkernel", (void*)k_pthread_yield, 0},
    {"scePthreadSetprio", "libkernel", (void*)k_setprio, 0},
    {"scePthreadGetprio", "libkernel", (void*)k_getprio, 0},
    {"scePthreadSetaffinity", "libkernel", (void*)k_setaffinity, 0},
    {"scePthreadGetaffinity", "libkernel", (void*)k_getaffinity, 0},
    {"scePthreadMutexattrInit", "libkernel", (void*)k_mutexattr_init, 0},
    {"scePthreadMutexattrDestroy", "libkernel", (void*)k_mutexattr_destroy, 0},
    {"scePthreadMutexattrSettype", "libkernel", (void*)k_mutexattr_settype, 0},
    {"scePthreadMutexattrSetprotocol", "libkernel", (void*)k_mutexattr_setprotocol, 0},
    {"scePthreadMutexInit", "libkernel", (void*)k_mutex_init, 0},
    {"scePthreadMutexDestroy", "libkernel", (void*)k_mutex_destroy, 0},
    {"scePthreadMutexLock", "libkernel", (void*)k_mutex_lock, 0},
    {"scePthreadMutexTrylock", "libkernel", (void*)k_mutex_trylock, 0},
    {"scePthreadMutexUnlock", "libkernel", (void*)k_mutex_unlock, 0},
    {"scePthreadCondInit", "libkernel", (void*)k_cond_init, 0},
    {"scePthreadCondDestroy", "libkernel", (void*)k_cond_destroy, 0},
    {"scePthreadCondWait", "libkernel", (void*)k_cond_wait, 0},
    {"scePthreadCondTimedwait", "libkernel", (void*)k_cond_timedwait, 0},
    {"scePthreadCondSignal", "libkernel", (void*)k_cond_signal, 0},
    {"scePthreadCondBroadcast", "libkernel", (void*)k_cond_broadcast, 0},
    {"scePthreadRwlockInit", "libkernel", (void*)k_rw_init, 0},
    {"scePthreadRwlockDestroy", "libkernel", (void*)k_rw_destroy, 0},
    {"scePthreadRwlockRdlock", "libkernel", (void*)k_rw_rdlock, 0},
    {"scePthreadRwlockWrlock", "libkernel", (void*)k_rw_wrlock, 0},
    {"scePthreadRwlockTryrdlock", "libkernel", (void*)k_rw_tryrdlock, 0},
    {"scePthreadRwlockTrywrlock", "libkernel", (void*)k_rw_trywrlock, 0},
    {"scePthreadRwlockUnlock", "libkernel", (void*)k_rw_unlock, 0},
    {"scePthreadOnce", "libkernel", (void*)k_once, 0},
    {"scePthreadKeyCreate", "libkernel", (void*)k_key_create, 0},
    {"scePthreadKeyDelete", "libkernel", (void*)k_key_delete, 0},
    {"scePthreadSetspecific", "libkernel", (void*)k_setspecific, 0},
    {"scePthreadGetspecific", "libkernel", (void*)k_getspecific, 0},
    {"clock_gettime", "libkernel", (void*)k_clock_gettime, 0},
    {"sceKernelClockGettime", "libkernel", (void*)k_sceKernelClockGettime, 0},
    {"gettimeofday", "libkernel", (void*)k_gettimeofday, 0},
    {"sceKernelGettimeofday", "libkernel", (void*)k_sceKernelGettimeofday, 0},
    {"sceKernelUsleep", "libkernel", (void*)k_usleep, 0},
    {"nanosleep", "libkernel", (void*)k_nanosleep, 0},
    {"sceKernelGetProcessTime", "libkernel", (void*)k_getprocesstime, 0},
    {"sceKernelReadTsc", "libkernel", (void*)k_readtsc, 0},
    {"sceKernelGetTscFrequency", "libkernel", (void*)k_tscfreq, 0},
    {"sched_yield", "libkernel", (void*)k_sched_yield, 0},
    {"getpid", "libkernel", (void*)k_getpid, 0},
    {"getpagesize", "libkernel", (void*)k_getpagesize, 0},
};
const size_t kernel_symbol_count = sizeof kernel_symbols / sizeof *kernel_symbols;
__attribute__((constructor)) static void kernel_init(void) { clock_gettime(CLOCK_MONOTONIC, &g_start); }

// ---- descriptor numbers ----------------------------------------------------------------------------------
// Files and kernel event queues share one descriptor number space (0-2 are the standard streams).
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_fd_used[4096];
int rt_fd_alloc(void) {
    pthread_mutex_lock(&g_fd_lock);
    for (int fd = 3; fd < (int)sizeof g_fd_used; ++fd)
        if (!g_fd_used[fd]) { g_fd_used[fd] = 1; pthread_mutex_unlock(&g_fd_lock); return fd; }
    pthread_mutex_unlock(&g_fd_lock);
    rt_logf("descriptor table exhausted");
    return -1;
}
void rt_fd_free(int fd) {
    pthread_mutex_lock(&g_fd_lock);
    if (fd >= 3 && fd < (int)sizeof g_fd_used) g_fd_used[fd] = 0;
    pthread_mutex_unlock(&g_fd_lock);
}

// ---- native-ABI accessors for the GPU library (runtime/jstars_gpu_host.c) -----------------------------
int* rt_errno_cell(void) { return &t_errno; }
uint64_t rt_read_tsc(void) { return k_readtsc(); }
uint64_t rt_tsc_frequency(void) { return k_tscfreq(); }
uint64_t rt_process_time_us(void) { return k_getprocesstime(); }
// The SDK version the game was built against: sce_process_param { u64 size; u32 magic 'ORBI'; u32 count; u32 sdk_version }.
static int g_sdk_version_test = -1; // self-test only: there is no game image (rt_set_test_sdk_version)
void rt_set_test_sdk_version(int v) { g_sdk_version_test = v; }
int rt_compiled_sdk_version(void) {
    if (g_sdk_version_test >= 0) return g_sdk_version_test;
    const uint8_t* pp = (const uint8_t*)rt_procparam();
    if (!pp) return -1;
    uint32_t magic, sdk;
    memcpy(&magic, pp + 8, 4);
    if (magic != 0x4942524Fu) return -1;
    memcpy(&sdk, pp + 0x10, 4);
    return (int)sdk;
}
