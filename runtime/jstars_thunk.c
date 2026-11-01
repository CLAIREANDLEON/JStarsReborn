// First-call tracing thunks for host-implemented imports.
//
// Every function import that the runtime (or the GPU library) implements is bound to a small thunk:
//     thunk:  jmp [slot]            ; slot initially points at the per-import first-call stub
//     stub:   mov r11d, index ; jmp [common]
// The first call of an import runs rt_first_call, which logs the import (NID, name, library, who
// implements it, its audit status, the caller and the first argument) and rewrites the slot to the real
// function, so later calls cost one indirect jump. The result is a record of exactly which imports a run
// reached, in order, which is what the import audit (docs/IMPORT_AUDIT.md) is built from.
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *nid_lib, *name, *origin; void* target; int called; uint64_t calls, reported; } ThunkInfo;
static int g_counting; // --count-imports: slots stay on the slow path so every call is counted
static ThunkInfo* g_info;
static uint64_t* g_slots;      // [count] slots + 1 cell holding the common routine's address
static uint8_t* g_code;        // count * 32 bytes: stub at +0, thunk at +16
static size_t g_count;

// Per-thread call counts for the threads named like the game's audio middleware (diagnostics, --count-imports only).
#define TSLOTS 24
typedef struct { char name[32]; int used; uint32_t* counts; uint32_t* reported; } ThreadCounts;
static ThreadCounts g_tc[TSLOTS];
static __thread int t_tslot = -2; // -2 not yet looked at, -1 not tracked
static void count_thread_call(uint32_t index) {
    if (t_tslot == -2) {
        const char* nm = rt_current_thread_name();
        t_tslot = -1;
        if (!strncmp(nm, "CRI", 3) || !strncmp(nm, "Cri", 3) || !strncmp(nm, "AvPlayer", 8) || !strncmp(nm, "sp_game_server", 14)) {
            for (int i = 0; i < TSLOTS; ++i) {
                int expect = 0;
                if (__atomic_compare_exchange_n(&g_tc[i].used, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    snprintf(g_tc[i].name, sizeof g_tc[i].name, "%s", nm);
                    g_tc[i].counts = calloc(g_count ? g_count : 1, sizeof(uint32_t)); g_tc[i].reported = calloc(g_count ? g_count : 1, sizeof(uint32_t));
                    __atomic_store_n(&t_tslot, i, __ATOMIC_RELEASE);
                    break;
                }
            }
        }
    }
    if (t_tslot >= 0 && g_tc[t_tslot].counts) g_tc[t_tslot].counts[index]++;
}

extern void rt_first_call_common(void);
__asm__(
    ".text\n"
    ".globl rt_first_call_common\n"
    "rt_first_call_common:\n"
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  pushq %rdi\n  pushq %rsi\n  pushq %rdx\n  pushq %rcx\n  pushq %r8\n  pushq %r9\n  pushq %rax\n  pushq %r10\n  pushq %r11\n"
    "  subq $136, %rsp\n"
    "  movdqu %xmm0, 0(%rsp)\n  movdqu %xmm1, 16(%rsp)\n  movdqu %xmm2, 32(%rsp)\n  movdqu %xmm3, 48(%rsp)\n"
    "  movdqu %xmm4, 64(%rsp)\n  movdqu %xmm5, 80(%rsp)\n  movdqu %xmm6, 96(%rsp)\n  movdqu %xmm7, 112(%rsp)\n"
    "  movl %r11d, %edi\n"
    "  movq 8(%rbp), %rsi\n"
    "  movq -8(%rbp), %rdx\n"
    "  call rt_first_call\n"
    "  movq %rax, %r11\n"
    "  movdqu 0(%rsp), %xmm0\n  movdqu 16(%rsp), %xmm1\n  movdqu 32(%rsp), %xmm2\n  movdqu 48(%rsp), %xmm3\n"
    "  movdqu 64(%rsp), %xmm4\n  movdqu 80(%rsp), %xmm5\n  movdqu 96(%rsp), %xmm6\n  movdqu 112(%rsp), %xmm7\n"
    "  addq $136, %rsp\n"
    "  addq $8, %rsp\n"
    "  popq %r10\n  popq %rax\n  popq %r9\n  popq %r8\n  popq %rcx\n  popq %rdx\n  popq %rsi\n  popq %rdi\n"
    "  popq %rbp\n"
    "  jmp *%r11\n");

GUEST_ABI uintptr_t rt_first_call(uint32_t index, uintptr_t caller, uint64_t a0) {
    ThunkInfo* t = &g_info[index];
    if (!__atomic_exchange_n(&t->called, 1, __ATOMIC_ACQ_REL)) {
        const char* note = "";
        const char* st = rt_audit_status(t->nid_lib, t->name, t->origin, &note);
        char where[96];
        rt_locate(caller, where, sizeof where);
        rt_logf("FIRST CALL #%u %s %s [%s: %s] [%s] from %s a0=0x%" PRIxPTR, index, t->name[0] ? t->name : "(unnamed)", t->nid_lib,
                t->origin, st, note, where, (uintptr_t)a0);
    }
    if (g_counting) { __atomic_add_fetch(&t->calls, 1, __ATOMIC_RELAXED); count_thread_call(index); }
    else __atomic_store_n(&g_slots[index], (uint64_t)(uintptr_t)t->target, __ATOMIC_RELEASE);
    return (uintptr_t)t->target;
}

size_t rt_thunk_bytes(size_t count) { return count * 32 + (count + 1) * 8; }

void rt_thunks_init(void* area, size_t count) {
    g_count = count;
    g_code = area;
    g_slots = (uint64_t*)(g_code + count * 32);
    g_info = calloc(count ? count : 1, sizeof *g_info);
    g_slots[count] = (uint64_t)(uintptr_t)rt_first_call_common;
}

void* rt_thunk_for(uint32_t index, void* target, const char* nid_lib, const char* name, const char* origin) {
    uint8_t* stub = g_code + (size_t)index * 32;
    uint8_t* thunk = stub + 16;
    ThunkInfo* t = &g_info[index];
    if (!t->target) {
        t->nid_lib = nid_lib; t->name = name; t->origin = origin; t->target = target;
        g_slots[index] = (uint64_t)(uintptr_t)stub;
        stub[0] = 0x41; stub[1] = 0xbb; memcpy(stub + 2, &index, 4); // mov r11d, index
        int32_t d = (int32_t)((uint8_t*)&g_slots[g_count] - (stub + 12));
        stub[6] = 0xff; stub[7] = 0x25; memcpy(stub + 8, &d, 4);      // jmp [rip+d] -> common routine cell
        int32_t d2 = (int32_t)((uint8_t*)&g_slots[index] - (thunk + 6));
        thunk[0] = 0xff; thunk[1] = 0x25; memcpy(thunk + 2, &d2, 4);  // jmp [rip+d2] -> slot
    }
    return thunk;
}

void rt_thunk_set_counting(int on) { g_counting = on; }
typedef struct { uint64_t d; size_t i; } Delta;
static int delta_cmp(const void* a, const void* b) { const Delta *x = a, *y = b; return x->d < y->d ? 1 : x->d > y->d ? -1 : 0; }
// Logs the imports called most since the previous report (diagnostics: what is the game doing right now).
void rt_thunk_report(void) {
    if (!g_counting || !g_info) return;
    Delta* ds = malloc((g_count ? g_count : 1) * sizeof *ds);
    size_t n = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint64_t c = __atomic_load_n(&g_info[i].calls, __ATOMIC_RELAXED), d = c - g_info[i].reported;
        g_info[i].reported = c;
        if (d) { ds[n].d = d; ds[n].i = i; n++; }
    }
    qsort(ds, n, sizeof *ds, delta_cmp);
    rt_logf("import calls since the last report (%zu distinct imports; top %d):", n, n < 14 ? (int)n : 14);
    for (size_t j = 0; j < n && j < 14; j++)
        rt_logf("  %8" PRIu64 "  %s (%s)", ds[j].d, g_info[ds[j].i].name[0] ? g_info[ds[j].i].name : "(unnamed)", g_info[ds[j].i].nid_lib);
    free(ds);
    // what the audio middleware's threads called (top 6 each)
    for (int k = 0; k < TSLOTS; ++k) {
        ThreadCounts* tc = &g_tc[k];
        if (!tc->used || !tc->counts) continue;
        Delta* d2 = malloc((g_count ? g_count : 1) * sizeof *d2);
        size_t m = 0; uint64_t total = 0;
        for (size_t i = 0; i < g_count; i++) {
            uint32_t d = tc->counts[i] - tc->reported[i];
            tc->reported[i] = tc->counts[i];
            if (d) { d2[m].d = d; d2[m].i = i; m++; total += d; }
        }
        qsort(d2, m, sizeof *d2, delta_cmp);
        char line[640]; size_t l = 0;
        for (size_t j = 0; j < m && j < 6; j++) l += (size_t)snprintf(line + l, sizeof line - l, "%s%s x%" PRIu64, j ? ", " : "", g_info[d2[j].i].name[0] ? g_info[d2[j].i].name : "(unnamed)", d2[j].d);
        rt_logf("  thread [%s]: %" PRIu64 " import calls: %s", tc->name, total, m ? line : "(none)");
        free(d2);
    }
}
