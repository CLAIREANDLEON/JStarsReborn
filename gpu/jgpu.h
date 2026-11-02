/* C interface of the GPU library (gpu/): shadPS4's Liverpool command processor, shader recompiler and
 * Vulkan renderer, GnmDriver, VideoOut and kernel event queues, hosted by the JStarsReborn runtime.
 * GPL-2.0-or-later (vendored shadPS4 b4e7ae77; see gpu/VENDOR.md). */
#ifndef JGPU_H
#define JGPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct JgpuHost {
    void (*log)(const char* line);                     /* one formatted log line */
    void (*fatal)(const char* what);                   /* reports a fatal GPU error; must not return */
    int (*protect)(void* addr, size_t len, int prot);  /* host page protection; prot bits 1=R 2=W 4=X; 0 on success */
    size_t (*safe_read)(void* dst, const void* src, size_t len); /* bytes copied before an unmapped page */
    int (*fd_alloc)(void);                             /* kernel descriptor number for an event queue */
    void (*fd_free)(int fd);
    void (*window_closed)(void);                       /* the player closed the game window */
    int* (*errno_cell)(void);                          /* the calling thread's guest-visible errno cell (what __error returns) */
    uint64_t (*read_tsc)(void);                        /* sceKernelReadTsc */
    uint64_t (*tsc_frequency)(void);                   /* sceKernelGetTscFrequency */
    uint64_t (*process_time_us)(void);                 /* sceKernelGetProcessTime */
    int (*compiled_sdk_version)(void);                 /* SDK version the game was built with, or a negative value if unknown */
    int (*is_neo_mode)(void);                          /* 1 when the console identity reported to the game is a PS4 Pro */
} JgpuHost;

typedef struct JgpuConfig {
    const char* title;       /* window title */
    const char* data_dir;    /* per-run directory for GPU logs/dumps; never the game folder */
    const char* capture_dir; /* PNG captures of presented frames */
    int32_t width, height;   /* initial window size */
    int headless_window;     /* 1: no window thread (unit tests of the library only) */
} JgpuConfig;

typedef struct JgpuStats {
    uint64_t draws, dispatches, flips_requested, gnm_frames, mem_maps;
} JgpuStats;

void jgpu_set_host(const JgpuHost* host);
/* Creates the window, Vulkan device, presenter and command processor, and registers the GnmDriver,
 * VideoOut and event-queue exports. Returns 0 on success; on failure the reason is logged. */
int jgpu_init(const JgpuConfig* config);
/* Function for an import "NID#library" (e.g. "0H2vBYbTLHI#libSceGnmDriver"), or NULL. */
void* jgpu_resolve(const char* nid_lib);
const char* jgpu_symbol_name(const char* nid_lib); /* C++ function name of an export, for diagnostics */
unsigned jgpu_symbol_count(void);
/* Page-tracking fault: native is the host exception context (EXCEPTION_POINTERS* / ucontext_t*).
 * Returns 1 when the GPU library handled the fault and the faulting instruction can be retried. */
int jgpu_handle_fault(void* native, uintptr_t address);
/* Guest memory was mapped / unmapped (GPU-visible ranges are tracked by the renderer). */
void jgpu_mem_map(uintptr_t addr, uint64_t len);
void jgpu_mem_unmap(uintptr_t addr, uint64_t len);
void jgpu_request_capture(int count); /* PNG of the next presented frame(s) into capture_dir */
int jgpu_window_focused(void);        /* 1 when the game window is the foreground window (keyboard input source) */
void jgpu_get_stats(JgpuStats* out);
/* AMD FSR SDK capability probe (jgpu_fsr.cpp): loads amd_fidelityfx_loader_vk.dll if present and lists the upscaler and
 * frame-generation versions for this device. Returns 0 if the loader answered; the summary is in jgpu_fsr_report(). */
int jgpu_fsr_probe(void);
const char* jgpu_fsr_report(void);
void jgpu_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
