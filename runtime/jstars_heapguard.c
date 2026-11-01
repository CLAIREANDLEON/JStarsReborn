// Debug-only guard-page allocator for the Windows x64 build (compile with -DJSTARS_HEAPGUARD and link with
// -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free; the Makefile target jstars-boot-gpu-hg.exe does both).
//
// Why: an intermittent fault inside ntdll's heap free routine (reported by the Windows verification runs, e.g. through
// k_attr_destroy -> free) is the signature of a heap-metadata overwrite. The Windows low-fragmentation heap randomises
// where small blocks go, so the same overflow corrupts a neighbour only some of the time. This allocator makes the
// overflow deterministic: every block sits at the END of its own pages, followed by a reserved no-access page (and
// preceded by one), and a freed block is decommitted and its address range is never reused. The first stray write
// past a block, the first access to a freed block and the first invalid or repeated free therefore fault at the
// offending instruction, where the runtime's fault handler already prints the module+offset and the host stack;
// rt_heapguard_describe() adds which allocation (size, allocating caller) the faulting address belongs to.
//
// Not used by default builds: it spends 3+ pages per allocation, so it is meant for short diagnostic runs
// (--stop-at-first-gnm, a few seconds of a game run), never for performance runs. Memory obtained from the C runtime
// itself (strdup and friends) is not in this table; freeing it is forwarded to the real free().
#ifdef JSTARS_HEAPGUARD
#include "jstars_rt.h"
#include "jstars_os.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define PG 4096u
typedef struct { uintptr_t base; size_t pages; uintptr_t user; size_t size; void* caller; uint32_t state; } Rec; // state 1 live, 2 freed
extern void* __real_malloc(size_t);
extern void __real_free(void*);
extern void* __real_realloc(void*, size_t);

static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_init;
static uintptr_t g_arena, g_arena_end, g_next;
static Rec* g_recs;
static size_t g_nrecs, g_recs_committed;
#define ARENA_BYTES (256ull << 30)
#define MAX_RECS (8u << 20)

static void hg_init(void) {
    // A fixed high address, far from the guest image (0x200000000) and the guest address window (0x800000000 ...).
    void* a = VirtualAlloc((void*)0x500000000000ull, ARENA_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    if (!a) a = VirtualAlloc(NULL, ARENA_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    g_recs = VirtualAlloc(NULL, (size_t)MAX_RECS * sizeof(Rec), MEM_RESERVE, PAGE_READWRITE);
    if (!a || !g_recs) { fputs("heapguard: cannot reserve the guard arena\n", stderr); ExitProcess(9); }
    g_arena = g_next = (uintptr_t)a; g_arena_end = g_arena + ARENA_BYTES;
    g_init = 1;
}

// Records are appended in increasing address order, so the table is sorted by 'base'.
static Rec* find_containing(uintptr_t addr) {
    size_t lo = 0, hi = g_nrecs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_recs[mid].base + (g_recs[mid].pages + 2) * PG <= addr) lo = mid + 1; else hi = mid;
    }
    return lo < g_nrecs && addr >= g_recs[lo].base ? &g_recs[lo] : NULL;
}
static Rec* find_user(uintptr_t p) {
    Rec* r = find_containing(p);
    return r && r->user == p ? r : NULL;
}

static void* hg_alloc(size_t n, void* caller) {
    size_t sz = (n + 15) & ~(size_t)15;
    if (!sz) sz = 16;
    size_t pages = (sz + PG - 1) / PG;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_init) hg_init();
    if (g_nrecs >= MAX_RECS || g_next + (pages + 2) * PG > g_arena_end) { ReleaseSRWLockExclusive(&g_lock); return NULL; }
    if ((g_nrecs + 1) * sizeof(Rec) > g_recs_committed) {
        size_t want = g_recs_committed + (4u << 20);
        if (!VirtualAlloc((char*)g_recs + g_recs_committed, want - g_recs_committed, MEM_COMMIT, PAGE_READWRITE)) { ReleaseSRWLockExclusive(&g_lock); return NULL; }
        g_recs_committed = want;
    }
    uintptr_t base = g_next;
    g_next += (pages + 2) * PG;
    uintptr_t data = base + PG;
    if (!VirtualAlloc((void*)data, pages * PG, MEM_COMMIT, PAGE_READWRITE)) { ReleaseSRWLockExclusive(&g_lock); return NULL; }
    uintptr_t user = data + pages * PG - sz;   // the block ends exactly at the trailing guard page
    Rec* r = &g_recs[g_nrecs++];
    r->base = base; r->pages = pages; r->user = user; r->size = n; r->caller = caller; r->state = 1;
    if (user > data) memset((void*)data, 0xA5, user - data); // the unused lead-in: a write below the block lands here
    ReleaseSRWLockExclusive(&g_lock);
    return (void*)user;
}

static void hg_report(const char* what, void* p, Rec* r) {
    char w[200];
    rt_logf("HEAPGUARD: %s of %p (block of %zu bytes allocated from %s%s)", what, p, r ? r->size : (size_t)0,
            r ? os_where((uintptr_t)r->caller, w, sizeof w) : "an unknown allocation", r && r->state == 2 ? ", already freed" : "");
    fflush(NULL);
    *(volatile char*)g_arena = 1; // the first guard page is never committed: an access violation here reaches the runtime's fault handler (module+offset, registers, host stack)
}

static int hg_free(void* p) { // 1 = handled here, 0 = not ours
    uintptr_t a = (uintptr_t)p;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_init || a < g_arena || a >= g_arena_end) { ReleaseSRWLockExclusive(&g_lock); return 0; }
    Rec* r = find_user(a);
    if (!r || r->state != 1) { Rec snap = r ? *r : (Rec){0}; ReleaseSRWLockExclusive(&g_lock); hg_report(r ? "second free" : "free of an address that is not a block start", p, r ? &snap : find_containing(a)); return 1; }
    uintptr_t data = r->base + PG;
    // underflow check: the lead-in below the block must still hold the fill pattern
    for (uintptr_t q = data; q < r->user; ++q)
        if (*(volatile uint8_t*)q != 0xA5) { Rec snap = *r; ReleaseSRWLockExclusive(&g_lock); hg_report("free after a write below the block", p, &snap); return 1; }
    r->state = 2;
    VirtualFree((void*)data, r->pages * PG, MEM_DECOMMIT);  // later access -> access violation; address range is never reused
    ReleaseSRWLockExclusive(&g_lock);
    return 1;
}

void* __wrap_malloc(size_t n) { return hg_alloc(n, __builtin_return_address(0)); }
void* __wrap_calloc(size_t a, size_t b) {
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) return NULL;
    return hg_alloc(n, __builtin_return_address(0)); // freshly committed pages are zero
}
void __wrap_free(void* p) { if (p && !hg_free(p)) __real_free(p); }
void* __wrap_realloc(void* p, size_t n) {
    if (!p) return hg_alloc(n, __builtin_return_address(0));
    AcquireSRWLockExclusive(&g_lock);
    Rec* r = g_init && (uintptr_t)p >= g_arena && (uintptr_t)p < g_arena_end ? find_user((uintptr_t)p) : NULL;
    size_t old = r ? r->size : 0;
    int ours = r != NULL;
    ReleaseSRWLockExclusive(&g_lock);
    if (!ours) return __real_realloc(p, n);
    if (!n) { hg_free(p); return NULL; }
    void* q = hg_alloc(n, __builtin_return_address(0));
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    hg_free(p);
    return q;
}

// Which guarded allocation does an address belong to (for the fault report); writes "" when none.
void rt_heapguard_describe(uintptr_t addr, char* out, size_t cap) {
    out[0] = 0;
    Rec snap = {0}; int have = 0, inside = 0;
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    if (g_init && addr >= g_arena && addr < g_arena_end) {
        inside = 1;
        Rec* r = find_containing(addr);
        if (r) { snap = *r; have = 1; }
    }
    ReleaseSRWLockExclusive(&g_lock);   // os_where may allocate: never call it with the lock held
    if (have) {
        char w[200];
        const char* where = os_where((uintptr_t)snap.caller, w, sizeof w);
        const char* part = addr < snap.base + PG ? "the guard page BELOW" : addr >= snap.base + (snap.pages + 1) * PG ? "the guard page ABOVE (a write or read past the end of)" : snap.state == 2 ? "(a freed block:) use after free of" : "the pages of";
        snprintf(out, cap, "address is in %s a %zu-byte block at %p allocated from %s%s", part, snap.size, (void*)snap.user, where, snap.state == 2 ? " [freed]" : "");
    } else if (inside) snprintf(out, cap, "address is inside the guard arena but outside any block");
}
#else
typedef int jstars_heapguard_unused;
#endif
