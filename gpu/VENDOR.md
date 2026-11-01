# GPU library provenance, dependencies and integration boundary

The `gpu/` library hosts the PlayStation 4 graphics stack for the native runtime: the Liverpool command
processor (PM4), the GCN shader recompiler (GCN -> SPIR-V), the Vulkan renderer and presenter, the GnmDriver and
VideoOut exports, and the kernel event queues. It is **GPL-2.0-or-later** code from shadPS4 plus our integration
layer; any binary that links it is distributed under GPL-2.0-or-later (see `../THIRD_PARTY_NOTICES.md`).

## Pinned upstream revision
* Repository: https://github.com/shadps4-emu/shadPS4 (GPL-2.0-or-later)
* Commit: `b4e7ae776071ceb1dd35004577a8719481e1d258` ("Added python3 (#5102)", 2026-09-24)
* Why this revision: it is the revision the approved reference port (HayatoG/iss_pc, `port/gpu/VENDOR.txt`) vendors, so
  its integration boundary (which core headers the video core needs, and what a replacement must offer) is known to build.
  Upstream has moved 265 commits since (checked 2026-10-31 against `55a97e46`); recompiler correctness fixes made after
  this revision are **not** applied. Candidate follow-ups are listed in iss_pc's VENDOR.txt; each must be a separate,
  evidence-backed change.
* Unmodified copies of: `src/video_core`, `src/shader_recompiler`, `src/common` (subset), `src/core/libraries/{gnmdriver,videoout}`,
  `src/core/libraries/kernel/{equeue.cpp,equeue.h,orbis_error.h,posix_error.h,time.h,process.h}`, `src/core/libraries/error_codes.h`,
  `src/core/platform.h`, `src/core/libraries/system/{userservice,systemservice}.h` under `vendor/shadps4/`.
* Local modifications to vendored files are listed in `patches/` (unified diffs against the pinned commit; regenerate with
  `tools/make_patches.sh`).
  What each of the nine modified files changes (all are in `patches/shadps4-local.patch`; checked against the diff on 2026-11-01):
  | File | Change | Why |
  |---|---|---|
  | `common/thread.cpp` | the fiber/pthread includes are dropped; Windows thread-description calls are resolved at run time | no emulator thread layer; mingw-w64 11 headers lack `SetThreadDescription` |
  | `shader_recompiler/.../emit_spirv.cpp` | one include (`<mutex>`) | missing transitively with our libstdc++ |
  | `shader_recompiler/.../spirv_emit_context.h` | explicit-object ("deducing this") members replaced by const/non-const overloads | GCC 13 does not implement C++23 deducing-this |
  | `video_core/amdgpu/liverpool.cpp` | PM4 trace (ring of the last packet headers, raw dwords around a failing packet and a copy of the failing buffer next to the log); every graphics submission is logged and the first six dcb buffers are saved; draw trace (`JGPU_TRACE_DRAWS`); **correctness:** a packet's length is read once before the packet is processed (the guest may reuse the command buffer as soon as a packet signals completion) | diagnostics from the run folder alone; the length fix is a genuine behavioural change to the vendored code |
  | `video_core/page_manager.cpp` | one include (`common/thread.h`) | missing transitively |
  | `renderer_vulkan/vk_presenter.cpp/.h` | ImGui composition replaced by a letterboxed, aspect-preserving blit of the game frame. **0.3.x:** frames are dropped, with a rate-limited warning, while the surface reports a 0x0 extent; degenerate sizes no longer reach `SetExpectedGameSize`; a failed frame-image allocation logs its dimensions | a minimised/hidden window made `vmaCreateImage` fail with `ErrorInitializationFailed` and ended Windows run 20261101-060040 with GPU FATAL (exit 5) |
  | `renderer_vulkan/vk_swapchain.cpp/.h` | `SurfaceUsable()` (false while the surface extent is 0x0) | used by the presenter guard above |
  | `texture_cache/image.cpp` | block-compressed images are created without storage usage when the device reports that combination unsupported but supports the format without it (AMD RX 9070 XT: BC5, BC6H; `tools/vkprobe`) | removes the logged `Bc5UnormBlock ... not supported` errors; the create info now matches what the device reported (0.4.0) |
  | `texture_cache/image_view.cpp` | a storage view of such a block-compressed image logs an error instead of silently creating an invalid view | visibility of the case above (never observed in the recorded runs) |

## Third-party code (vendor/externals), all GPL-compatible
| Component | Revision | License |
|---|---|---|
| Vulkan-Headers | ee3b5caa | Apache-2.0 / MIT |
| VulkanMemoryAllocator | f378e7b3 | MIT |
| sirit (SPIR-V emitter) + SPIRV-Headers | c58f4d44 / 2acb319a | BSD-2-Clause / MIT |
| fmt (shadps4 fork) | ec73fb72 | MIT |
| robin-map | 4ec1bf19 | MIT |
| xbyak | 44a72f36 | BSD-3-Clause |
| magic_enum | a413fcc9 | MIT |
| half | 1ddada22 | MIT |
| xxHash | 953a09ab | BSD-2-Clause |
| Zydis + Zycore | 120e0e70 / 38d4f028 | MIT |
| stb_image_write | (as in shadPS4) | MIT / public domain |
| renderdoc_app.h | (as in shadPS4) | MIT |
| gcn register headers | (in-tree in shadPS4) | GPL-2.0-or-later |
| Boost (ICL, Asio headers; header-only) | system 1.83 | BSL-1.0 |
Build-time tool: `glslangValidator` (glslang 15.1, BSD-3-Clause) generates the host shaders; it is not shipped.

## Integration boundary (what replaces shadPS4's emulator)
The vendored code expects the shadPS4 "core" around it. This port supplies, in `shim/` and `jgpu*.cpp`:
| Upstream expectation | Replacement |
|---|---|
| `core/memory.h` MemoryManager, `core/address_space.h` | `shim/core/memory.h`: guest addresses are host addresses; map/unmap notifications come from the runtime (`jgpu_mem_map`), page protection and safe reads through `JgpuHost` callbacks |
| `core/signals.h` fault dispatch | `shim/core/signals.h` registry; the runtime's fault handler calls `jgpu_handle_fault` first |
| `core/emulator_settings.h` | `shim/core/emulator_settings.h`: `JSTARS_*` environment variables |
| `core/file_sys/fs.h` HandleTable (event queues) | `shim/core/file_sys/fs.h`: descriptor numbers come from the runtime |
| spdlog logging, `common/assert.cpp` | `shim/common/logging`, `shim/common/assert.cpp`: log lines go to the runtime log; failed assertions are fatal (exit 5) |
| Tracy, ImGui, devtools, notifications | no-op shims; the presenter's ImGui composition is replaced by a letterboxed blit (patch `0001`) |
| SDL window | `Frontend::WindowSDL` implemented natively (`jgpu_window_win32.cpp`, `jgpu_window_x11.cpp`) |
| `common/path_util.cpp` | `shim/common/path_util_impl.cpp`: every path is below the per-run data directory |
| miniz pipeline archive (`cache_storage.cpp`) | stub (no on-disk cache in this checkpoint) |
| emulator.cpp library registration | `jgpu_init`: window, then `Kernel::RegisterEventQueue`, `GnmDriver::RegisterLib`, `VideoOut::RegisterLib` |
`shim/core/{libraries/libs.h,libraries/kernel/kernel.h,debug_state.h,signals.h,devtools/widget/gpu_memory.h}` and
`shim/imgui/renderer/*` follow the shims of the reference port (HayatoG/iss_pc `port/gpu/shim`, GPL-2.0-or-later); the
other shim files are written for this project.
