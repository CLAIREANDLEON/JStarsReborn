// Kernel event flags (sceKernelCreateEventFlag family) and the small process-level services that go with
// them (exit, stack-protector failure, time zone, resource usage, signal masks).
// Event flag semantics follow the console contract as documented by the SDK and as implemented by the
// reference emulator: a set wakes every waiter whose condition is met, each applying its own clear mode in
// queue order; Clear ANDs the flag with the argument; a timed-out wait writes 0 to the timeout.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;
#define SCE_ERR(e) ((int)(0x80020000u | (unsigned)(e)))
enum { K_EPERM = 1, K_ESRCH = 3, K_EACCES = 13, K_EBUSY = 16, K_EINVAL = 22, K_EAGAIN = 35, K_ETIMEDOUT = 60, K_ENAMETOOLONG = 63, K_ECANCELED = 85 };

typedef struct Waiter {
    struct Waiter* next;
    pthread_cond_t cv;
    uint64_t bits, result;
    int and_mode, clear_mode;    // clear_mode: 0 none, 1 all, 2 pattern bits
    int prio;
    int ready, canceled, deleted;
} Waiter;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t drained;
    Waiter* head;
    uint64_t bits;
    int single, prio_queue, deleted;
    char name[32];
} Evf;

static int met(const Evf* e, const Waiter* w) { return w->and_mode ? (e->bits & w->bits) == w->bits : (e->bits & w->bits) != 0; }
static void apply_clear(Evf* e, const Waiter* w) { if (w->clear_mode == 1) e->bits = 0; else if (w->clear_mode == 2) e->bits &= ~w->bits; }
static void enqueue(Evf* e, Waiter* w) {
    Waiter** pp = &e->head;
    if (e->prio_queue) while (*pp && (*pp)->prio >= w->prio) pp = &(*pp)->next;
    else while (*pp) pp = &(*pp)->next;
    w->next = *pp; *pp = w;
}
static void dequeue(Evf* e, Waiter* w) {
    for (Waiter** pp = &e->head; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; break; }
    if (e->deleted && !e->head) pthread_cond_signal(&e->drained);
}

static int GUEST_ABI k_evf_create(Evf** out, const char* name, uint32_t attr, uint64_t init, const void* opt) {
    if (!out || !name || opt || attr > 0x32) return SCE_ERR(K_EINVAL);
    if (strlen(name) >= 32) return SCE_ERR(K_ENAMETOOLONG);
    Evf* e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->drained, NULL);
    e->bits = init;
    e->single = (attr & 0xf0) != 0x20;   // SINGLE (0x10) is also the default
    e->prio_queue = (attr & 0x0f) == 0x02;
    snprintf(e->name, sizeof e->name, "%s", name);
    *out = e;
    return 0;
}
static int GUEST_ABI k_evf_delete(Evf* e) {
    if (!e) return SCE_ERR(K_ESRCH);
    pthread_mutex_lock(&e->m);
    e->deleted = 1;
    for (Waiter* w = e->head; w; w = w->next) { w->deleted = 1; pthread_cond_signal(&w->cv); }
    while (e->head) pthread_cond_wait(&e->drained, &e->m);
    pthread_mutex_unlock(&e->m);
    pthread_mutex_destroy(&e->m); pthread_cond_destroy(&e->drained);
    free(e);
    return 0;
}
static int GUEST_ABI k_evf_set(Evf* e, uint64_t bits) {
    if (!e) return SCE_ERR(K_ESRCH);
    pthread_mutex_lock(&e->m);
    e->bits |= bits;
    for (Waiter* w = e->head; w; w = w->next) {
        if (w->ready || !met(e, w)) continue;
        w->result = e->bits;
        apply_clear(e, w);
        w->ready = 1;
        pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&e->m);
    return 0;
}
static int GUEST_ABI k_evf_clear(Evf* e, uint64_t bits) {
    if (!e) return SCE_ERR(K_ESRCH);
    pthread_mutex_lock(&e->m); e->bits &= bits; pthread_mutex_unlock(&e->m);
    return 0;
}
static int GUEST_ABI k_evf_cancel(Evf* e, uint64_t pattern, int* num_waiters) {
    if (!e) return SCE_ERR(K_ESRCH);
    pthread_mutex_lock(&e->m);
    int n = 0;
    for (Waiter* w = e->head; w; w = w->next) { ++n; w->result = pattern; w->canceled = 1; pthread_cond_signal(&w->cv); }
    if (num_waiters) *num_waiters = n;
    e->bits = pattern;
    pthread_mutex_unlock(&e->m);
    return 0;
}
static uint64_t mono_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000; }

static int wait_impl(Evf* e, uint64_t bits, uint32_t mode, uint64_t* result, uint32_t* timeout_us, int poll) {
    if (!e) return SCE_ERR(K_ESRCH);
    if (bits == 0) return SCE_ERR(K_EINVAL);
    Waiter w; memset(&w, 0, sizeof w);
    switch (mode & 0xf) { case 1: w.and_mode = 1; break; case 2: w.and_mode = 0; break; default: return SCE_ERR(K_EINVAL); }
    switch (mode & 0xf0) { case 0x00: w.clear_mode = 0; break; case 0x10: w.clear_mode = 1; break; case 0x20: w.clear_mode = 2; break; default: return SCE_ERR(K_EINVAL); }
    w.bits = bits;
    pthread_mutex_lock(&e->m);
    if (e->deleted) { pthread_mutex_unlock(&e->m); return SCE_ERR(K_EACCES); }
    if (e->single && e->head) { pthread_mutex_unlock(&e->m); return SCE_ERR(K_EPERM); }
    if (met(e, &w)) {
        if (result) *result = e->bits;
        apply_clear(e, &w);
        pthread_mutex_unlock(&e->m);
        return 0;
    }
    if (poll) { if (result) *result = e->bits; pthread_mutex_unlock(&e->m); return SCE_ERR(K_EBUSY); }
    pthread_condattr_t at; pthread_condattr_init(&at);
#ifndef _WIN32
    pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&w.cv, &at); pthread_condattr_destroy(&at);
    enqueue(e, &w);
    uint64_t t0 = mono_us(); int timed_out = 0;
    struct timespec dl;
    if (timeout_us) {
#ifdef _WIN32
        clock_gettime(CLOCK_REALTIME, &dl);
#else
        clock_gettime(CLOCK_MONOTONIC, &dl);
#endif
        dl.tv_sec += *timeout_us / 1000000u; dl.tv_nsec += (long)(*timeout_us % 1000000u) * 1000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    }
    while (!w.ready && !w.canceled && !w.deleted) {
        if (timeout_us) { if (pthread_cond_timedwait(&w.cv, &e->m, &dl) == ETIMEDOUT && !w.ready && !w.canceled && !w.deleted) { timed_out = 1; break; } }
        else pthread_cond_wait(&w.cv, &e->m);
    }
    dequeue(e, &w);
    uint64_t res = w.ready || w.canceled ? w.result : e->bits;
    pthread_mutex_unlock(&e->m);
    pthread_cond_destroy(&w.cv);
    if (result) *result = res;
    if (timed_out) { if (timeout_us) *timeout_us = 0; return SCE_ERR(K_ETIMEDOUT); }
    if (w.canceled) return SCE_ERR(K_ECANCELED);
    if (w.deleted) return SCE_ERR(K_EACCES);
    if (timeout_us) { uint64_t used = mono_us() - t0; *timeout_us = used >= *timeout_us ? 0 : *timeout_us - (uint32_t)used; }
    return 0;
}
static int GUEST_ABI k_evf_wait(Evf* e, uint64_t bits, uint32_t mode, uint64_t* res, uint32_t* timeout) { return wait_impl(e, bits, mode, res, timeout, 0); }
static int GUEST_ABI k_evf_poll(Evf* e, uint64_t bits, uint32_t mode, uint64_t* res) { return wait_impl(e, bits, mode, res, NULL, 1); }

// ---- process-level services ---------------------------------------------------------------------------------------
static void GUEST_ABI k_exit(int code) {
    rt_logf("guest called _exit(%d)", code);
    fflush(NULL);
    _exit(code);
}
static void GUEST_ABI k_stack_chk_fail(void) {
    rt_logf("FATAL: __stack_chk_fail called by the guest (stack smashing detected) from %p", __builtin_return_address(0));
    fflush(NULL);
    _exit(8);
}
typedef struct { int32_t tz_minuteswest, tz_dsttime; } KTimezone;
static int GUEST_ABI k_gettimezone(KTimezone* tz) {
    if (!tz) return SCE_ERR(K_EINVAL);
    time_t now = time(NULL);
    struct tm lt, gm;
#ifdef _WIN32
    localtime_s(&lt, &now); gmtime_s(&gm, &now);
#else
    localtime_r(&now, &lt); gmtime_r(&now, &gm);
#endif
    gm.tm_isdst = lt.tm_isdst;
    tz->tz_minuteswest = (int32_t)((mktime(&gm) - now) / 60);
    tz->tz_dsttime = lt.tm_isdst > 0;
    return 0;
}
typedef struct { int64_t utime_s, utime_us, stime_s, stime_us; int64_t rest[14]; } KRusage;
static int GUEST_ABI k_getrusage(int who, KRusage* ru) {
    (void)who;
    memset(ru, 0, sizeof *ru);
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); // user+system are not separable on this host: all reported as user time
    ru->utime_s = t.tv_sec; ru->utime_us = t.tv_nsec / 1000;
    return 0;
}
// Signal masks: the guest never receives host signals, so a mask is bookkeeping only. FreeBSD sigset_t is 16 bytes.
typedef struct { uint32_t bits[4]; } KSigset;
static __thread KSigset t_sigmask;
static int GUEST_ABI k_sigfillset(KSigset* s) { memset(s, 0xff, sizeof *s); return 0; }
static int GUEST_ABI k_sigprocmask(int how, const KSigset* set, KSigset* old) {
    if (old) *old = t_sigmask;
    if (!set) return 0;
    switch (how) {
    case 1: for (int i = 0; i < 4; ++i) t_sigmask.bits[i] |= set->bits[i]; break;    // SIG_BLOCK
    case 2: for (int i = 0; i < 4; ++i) t_sigmask.bits[i] &= ~set->bits[i]; break;   // SIG_UNBLOCK
    case 3: t_sigmask = *set; break;                                                // SIG_SETMASK
    default: *rt_errno_cell() = K_EINVAL; return -1;
    }
    return 0;
}

const HostSymbol evf_symbols[] = {
    {"sceKernelCreateEventFlag", "libkernel", (void*)k_evf_create, 0},
    {"sceKernelDeleteEventFlag", "libkernel", (void*)k_evf_delete, 0},
    {"sceKernelSetEventFlag", "libkernel", (void*)k_evf_set, 0},
    {"sceKernelClearEventFlag", "libkernel", (void*)k_evf_clear, 0},
    {"sceKernelCancelEventFlag", "libkernel", (void*)k_evf_cancel, 0},
    {"sceKernelWaitEventFlag", "libkernel", (void*)k_evf_wait, 0},
    {"sceKernelPollEventFlag", "libkernel", (void*)k_evf_poll, 0},
    {"_exit", "libkernel", (void*)k_exit, 0},
    {"__stack_chk_fail", "libkernel", (void*)k_stack_chk_fail, 0},
    {"sceKernelGettimezone", "libkernel", (void*)k_gettimezone, 0},
    {"getrusage", "libkernel", (void*)k_getrusage, 0},
    {"sigfillset", "libkernel", (void*)k_sigfillset, 0},
    {"sigprocmask", "libkernel", (void*)k_sigprocmask, 0},
    {"_sigprocmask", "libkernel", (void*)k_sigprocmask, 0},
};
const size_t evf_symbol_count = sizeof evf_symbols / sizeof *evf_symbols;
