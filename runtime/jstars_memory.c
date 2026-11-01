// Console memory model: "direct memory" is a physical-offset space backed by one shared file so
// that CPU mappings of the same offsets alias (the GPU path will rely on this); "flexible memory"
// is anonymous. Addresses are host addresses. Non-FIXED allocations are placed inside the console's
// 40-bit user space (os_va_window, jstars_os.c) because GPU descriptors store 40-bit addresses; a
// guarantee of addresses below 4 GiB (32-bit pointer packing) is not provided: none has been observed.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;
#define SCE_ERR(e) ((int)(0x80020000u | (unsigned)(e)))
#define SCE_EINVAL SCE_ERR(22)
#define SCE_ENOMEM SCE_ERR(12)
#define SCE_EACCES SCE_ERR(13)

enum { DIRECT_SIZE_GIB = 6 };
static const uint64_t k_direct_size = (uint64_t)DIRECT_SIZE_GIB << 30;
static int g_dm_ready;
static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t start, len; } Extent;
static Extent g_extents[4096];
static size_t g_extent_count;

static void dm_init(void) {
    if (!g_dm_ready) {
        if (os_dm_init(k_direct_size)) { rt_logf("direct memory backing store failed"); abort(); }
        g_dm_ready = 1;
    }
}
static int cmp_ext(const void* a, const void* b) { const Extent *x = a, *y = b; return x->start < y->start ? -1 : x->start > y->start; }

static uint64_t GUEST_ABI k_dm_size(void) { return k_direct_size; }

static int GUEST_ABI k_dm_allocate(uint64_t search_start, uint64_t search_end, uint64_t len, uint64_t align, int type, int64_t* out) {
    (void)type;
    if (!len || (len & 0x3fff) || !out) return SCE_EINVAL;
    if (!align) align = 0x4000;
    if (align < os_dm_granularity()) align = os_dm_granularity(); // a host may need coarser placement; larger alignment is always valid
    if (search_end == 0 || search_end > k_direct_size) search_end = k_direct_size;
    pthread_mutex_lock(&g_mem_lock);
    uint64_t pos = (search_start + align - 1) & ~(align - 1);
    qsort(g_extents, g_extent_count, sizeof *g_extents, cmp_ext);
    for (size_t i = 0; i <= g_extent_count; ++i) {
        uint64_t limit = i < g_extent_count ? g_extents[i].start : search_end;
        if (limit > search_end) limit = search_end;
        if (pos + len <= limit) {
            if (g_extent_count >= 4096) break;
            g_extents[g_extent_count++] = (Extent){pos, len};
            pthread_mutex_unlock(&g_mem_lock);
            *out = (int64_t)pos;
            return 0;
        }
        if (i < g_extent_count) { uint64_t e = g_extents[i].start + g_extents[i].len; if (e > pos) pos = (e + align - 1) & ~(align - 1); }
    }
    pthread_mutex_unlock(&g_mem_lock);
    return SCE_ENOMEM;
}
static int GUEST_ABI k_dm_release(uint64_t start, uint64_t len) {
    pthread_mutex_lock(&g_mem_lock);
    for (size_t i = 0; i < g_extent_count; ++i)
        if (g_extents[i].start == start && g_extents[i].len == len) { g_extents[i] = g_extents[--g_extent_count]; pthread_mutex_unlock(&g_mem_lock); return 0; }
    pthread_mutex_unlock(&g_mem_lock);
    rt_logf("sceKernelReleaseDirectMemory: no extent %#" PRIx64 "+%#" PRIx64 " (partial release is not implemented)", start, len);
    return SCE_EINVAL;
}

#define MAP_FIXED_SCE 0x10
#define MAP_NO_OVERWRITE_SCE 0x80
static int prot_of(int p) { return (p & 1 ? OS_PROT_R : 0) | (p & 2 ? OS_PROT_W : 0) | (p & 4 ? OS_PROT_X : 0); } // GPU bits (0x10/0x20) have no CPU meaning
static int mode_of(int sce_flags) { return !(sce_flags & MAP_FIXED_SCE) ? OS_MAP_ANYWHERE : (sce_flags & MAP_NO_OVERWRITE_SCE) ? OS_MAP_FIXED_NOREPLACE : OS_MAP_FIXED; }

static int GUEST_ABI k_dm_map(void** addr, uint64_t len, int prot, int flags, int64_t phys, uint64_t align) {
    (void)align;
    if (!addr || !len || (len & 0x3fff) || phys < 0 || (uint64_t)phys + len > k_direct_size) return SCE_EINVAL;
    dm_init();
    void* r = os_dm_map(*addr, len, prot_of(prot), (uint64_t)phys, mode_of(flags));
    if (!r) { rt_logf("MapDirectMemory(%p,%#" PRIx64 ",phys %#" PRIx64 ") failed", *addr, len, (uint64_t)phys); return SCE_ENOMEM; }
    *addr = r;
    rt_gpu_mem_map(r, len);
    return 0;
}
static int GUEST_ABI k_flex_map_named(void** addr, uint64_t len, int prot, int flags, const char* name) {
    (void)name;
    if (!addr || !len || (len & 0x3fff)) return SCE_EINVAL;
    void* r = os_map_anon(*addr, len, prot_of(prot), mode_of(flags));
    if (!r) { rt_logf("MapFlexibleMemory(%p,%#" PRIx64 ") failed", *addr, len); return SCE_ENOMEM; }
    *addr = r;
    rt_gpu_mem_map(r, len);
    return 0;
}
static int GUEST_ABI k_flex_map(void** addr, uint64_t len, int prot, int flags) { return k_flex_map_named(addr, len, prot, flags, ""); }
static int GUEST_ABI k_munmap_sce(void* addr, uint64_t len) {
    rt_gpu_mem_unmap(addr, len); // the renderer drops its tracking before the pages go away
    return os_unmap(addr, len) ? SCE_EINVAL : 0;
}

// libc-style mmap/munmap (FreeBSD flag values). Only anonymous private mappings and fixed
// placement are supported; anything else is refused loudly instead of guessed.
static void* GUEST_ABI k_mmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
    const int F_PRIVATE = 2, F_FIXED = 0x10, F_ANON = 0x1000, F_STACK = 0x400, F_NOCORE = 0x20000;
    (void)off;
    if (!(flags & F_ANON) || fd != -1 || (flags & ~(F_PRIVATE | F_FIXED | F_ANON | F_STACK | F_NOCORE | 1))) {
        rt_logf("mmap: unsupported flags %#x fd %d", flags, fd);
        return (void*)-1;
    }
    void* r = os_map_anon(addr, len, prot_of(prot), flags & F_FIXED ? OS_MAP_FIXED : OS_MAP_ANYWHERE);
    return r ? r : (void*)-1;
}
static int GUEST_ABI k_munmap(void* addr, size_t len) { rt_gpu_mem_unmap(addr, len); return os_unmap(addr, len); }
static int GUEST_ABI k_msync(void* addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; } // anonymous/shared-memory mappings are always coherent
static int GUEST_ABI k_mlock(const void* addr, size_t len) { (void)addr; (void)len; return 0; } // residency hint; pages are not pinned on this host

const HostSymbol memory_symbols[] = {
    {"sceKernelGetDirectMemorySize", "libkernel", (void*)k_dm_size, 0},
    {"sceKernelAllocateDirectMemory", "libkernel", (void*)k_dm_allocate, 0},
    {"sceKernelReleaseDirectMemory", "libkernel", (void*)k_dm_release, 0},
    {"sceKernelMapDirectMemory", "libkernel", (void*)k_dm_map, 0},
    {"sceKernelMapFlexibleMemory", "libkernel", (void*)k_flex_map, 0},
    {"sceKernelMapNamedFlexibleMemory", "libkernel", (void*)k_flex_map_named, 0},
    {"sceKernelMunmap", "libkernel", (void*)k_munmap_sce, 0},
    {"mmap", "libkernel", (void*)k_mmap, 0},
    {"munmap", "libkernel", (void*)k_munmap, 0},
    {"msync", "libkernel", (void*)k_msync, 0},
    {"sceKernelMlock", "libkernel", (void*)k_mlock, 0},
};
const size_t memory_symbol_count = sizeof memory_symbols / sizeof *memory_symbols;
