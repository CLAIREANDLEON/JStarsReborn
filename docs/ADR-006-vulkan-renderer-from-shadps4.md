# ADR-006: GPU stack derived from shadPS4 (Vulkan)

Status: accepted on the owner's direction (checkpoint 0.2.2 -> 0.3). Nothing is published.

## Context

The original game's code runs natively (ADR-005). Its graphics calls (libSceGnmDriver, libSceVideoOut) target the
console's GPU, which has no Windows equivalent: the game builds PM4 command buffers and GCN shader binaries and
submits them. Implementing 50 exports does not render anything; the command processor, shader translation,
GPU-memory tracking, synchronization and presentation have to exist together. The owner authorized reuse of
suitable open-source components, subject to licenses and attribution.

## Decision

* Vendor shadPS4's `video_core` (Liverpool PM4 processor, buffer/texture caches, Vulkan renderer, presenter),
  `shader_recompiler` (GCN -> SPIR-V via sirit), `gnmdriver`, `videoout` and the event-queue library at one pinned
  revision (b4e7ae77, see `gpu/VENDOR.md`), plus the libraries it needs (see the dependency table there).
* Replace the emulator around it: loader, memory manager, signal layer, window and settings are supplied by the
  JStarsReborn runtime through a small shim (`gpu/shim`, `gpu/jgpu*.cpp`) and a plain-C API (`gpu/jgpu.h`).
* Replace the ImGui-based presentation path with a plain blit of the game's frame to the window (patch recorded in
  `gpu/patches/shadps4-local.patch`). Genuine frame captures reuse upstream's screenshot path.
* Keep the original-code execution model: guest addresses are host addresses; the renderer reads guest memory
  directly and tracks CPU writes through page protection and the runtime's fault handler.
* Keep `--stop-at-first-gnm` (the verified 0.2.2 diagnostic) working and unchanged.

## Consequences

* The runtime and the GPU library are GPL-2.0-or-later as a whole (shadPS4 is GPL-2.0-or-later). The setup tool and
  the DirectX foundation remain separate programs; the combined work is not distributed yet.
* Parts that exist in upstream as stubs (`STUBBED`) stay stubs here; they are classified in `docs/IMPORT_AUDIT.md`
  and log at every call. A bound export is not a working export.
* Windows page-tracking faults in guest leaf functions can clobber the System V red zone (the reference port
  rewrites code statically to avoid this). Until that protection exists on Windows, GPU runs there are
  diagnostic-only; the Linux harness is the functional reference.
* Windows presents through Vulkan. The DX12 foundation (`JStarsReborn.exe`) is unchanged and not used by the game
  runtime; the D3D12 debug layer and symlink-fixture coverage stay separate checks.

## Update (checkpoint 0.3.x, 2026-11-01): what was learned while integrating

* **Guest address window.** The console's GPU registers and descriptors carry 40-bit guest addresses (a shader address is
  stored as address>>8 in a LO/HI pair and the SDK's own code requires HI == 0), but Windows hands out allocations above
  1 TiB. The runtime therefore places guest-visible memory inside a configured window (`os_va_window`, `runtime/jstars_os.c`);
  explicit fixed requests are honoured as given.
* **Compiler workaround.** MinGW GCC 13 hits internal compiler errors / SysV exception-handling problems on four of the GPU
  translation units that define console-ABI entry points; those units are compiled with clang (`gpu/CMakeLists.txt`,
  same target and headers). The same reason keeps struct-by-value out of `jstars_input.c`. This is a toolchain
  workaround, not a design choice, and it is the first thing to remove if the toolchain is updated.
* **Command-processor race.** A race between the PM4 processing thread and submission was found and fixed during bring-up
  (diagnosed from the PM4 trace and the submit dumps `gpu/pm4-submit-*.bin`).
* **Presentation.** Presentation is a letterboxed blit of the game's flip buffer (patch recorded in
  `gpu/patches/shadps4-local.patch`); frame captures reuse upstream's screenshot path, so a capture is the image the
  renderer produced for a game flip, not a window screenshot.
* **Red-zone risk (still open, no symptom).** The consequence stated above is unchanged: the protection against page-tracking
  faults clobbering the System V red zone does not exist on Windows. Recorded Windows runs (several minutes each, tens of
  thousands of submissions, up to a 620 s full Free Battle with 8.9 M draws) did not show the symptom. The one start-up
  access violation that had been attributed to it as a possibility (run `20261101-050748-PC-ace318`) was not
  reproduced and is **not** explained; see the next item for the crash that was.
* **Host objects the game's own libc writes into (fixed in build w).** About 2 % of Windows starts crashed in `free()` shortly
  after the game's libc init (`ntdll.dll+0x27381` via `scePthreadAttrDestroy`): 5/230 on build t, 8/430 on build v in the repeated start-up loops, and
  the same signature in all 13 of those crashes (and in the one inside run 873ce9). A debug-only guard-page allocator (`runtime/jstars_heapguard.c`, build target
  `jstars-boot-gpu-hg.exe`) trapped the cause deterministically on its first run: right after `scePthreadMutexInit` the game's
  libc executes `or dword [obj+0x20], 0x100` (libc.prx+0x23a20) on the object the mutex slot points to. On the console that
  object is a libkernel-internal structure; here it was a bare 8-byte winpthreads mutex, so the write landed past the block
  and corrupted the Windows heap (on Linux `sizeof(pthread_mutex_t)` is 40, which hid it). Mutex, condition-variable,
  read/write-lock and attribute slots now point at a zeroed block with 256 guest-private bytes in front of the host primitive
  (`guest_obj_new`, `runtime/jstars_kernel.c`). Evidence: build w 0/200 start-up failures against 3/200 for build v in the same
  interleaved loop (if the old ~2 % rate still applied, seeing no failure in 200 starts would have about a 1-2 % chance) (`evidence/windows-gpu-0.3.0/startup-crash-investigation/`), the heap-guarded build 0/200 on the same
  path and a 60 s heap-guarded game run (1,799 flips) that trapped nothing. This does not prove there is no other host heap
  error: the guard run covers the start-up path and one minute of the game, not a full match. It is also unknown whether
  the ace318 crash was this bug.
* **Open renderer finding.** 68-71 times per 10-minute battle run the renderer logs
  `image.cpp:166 Image: image format Bc5UnormBlock type 2D is not supported (... usage { ... Storage })` (the driver query for a
  BC5 image created with the storage-usage flag fails on this GPU); image creation continues and the captured frames look
  correct, but whether a texture is wrong anywhere has not been checked. The Vulkan validation layer's 4 GiB-buffer VUID
  is also still uninvestigated.
* **Not shadPS4's job.** Audio (`sceAudioOut*` to WASAPI), input (scripted/XInput/keyboard), the AvPlayer protocol and
  save data are runtime replacements (`runtime/jstars_av.c`, `jstars_audiohost.c`, `jstars_input.c`, `jstars_avplayer.c`,
  `jstars_platform.c`), not part of this stack; see `docs/IMPORT_AUDIT.md` for their classification.
