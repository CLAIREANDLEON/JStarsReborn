# Frame-generation integration checkpoint

`gpu/jgpu_frame_generation.{h,cpp}` implements a standalone Vulkan command recorder
against the pinned FidelityFX SDK 2.4.0 headers, including the Frame Generation
4.0.2 `PrepareV2` interface. **It is not connected to the game renderer or included
in the runtime build yet.** No in-game frame generation or performance improvement
is claimed by this checkpoint.

AMD documents FG 4.0.2 for Windows 11, Radeon RX 9000-series or newer, and Vulkan
1.2 or newer. Capability enumeration on the actual device remains authoritative;
an SDK version number alone does not establish support. See the pinned
[FG 4.0.2 documentation](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/docs/techniques/frame-interpolation-ml.md)
and [integration API](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/docs/techniques/frame-interpolation-api.md).

## Implemented

- Device-specific provider enumeration, explicit opaque ID override, and a
  post-creation query that rejects a provider mismatch. The caller chooses the
  enumerated FG 4 provider; array order and ID bit patterns are not version tests.
  The module can also use an explicitly selected FG 3 provider, but does not
  automatically fall back or label it FG 4. Names come from the SDK.
- Persistent creation descriptors: Vulkan backend, API version opt-in and version
  override remain alive until context destruction. Copying/moving a context is
  prohibited because the descriptor chain contains self-pointers.
- Per real frame: `Configure` -> `PrepareV2` -> `Generate`, with matching frame IDs,
  one generated output, and the caller's command buffer. Async workloads and SDK
  swapchain notification are disabled. No submission, presentation, sleep, game
  patch, or simulation update occurs here.
- Validation before SDK calls: required images, dimensions, formats, resource
  states, separate handles, camera basis, camera ranges, finite timing, explicit
  motion-vector scale, transfer function and content rectangle. This checks the
  supplied metadata; it cannot prove the images or camera values are authentic.
- Reset on first use, skipped/repeated/wrapped frame IDs, render-size/content-rect
  changes, explicit cuts and invalid input. SDK errors stop subsequent stages and
  require context recreation. `reset_history()` cannot clear an SDK failure.
- `Result::interpolated` stays false on reset frames and errors. A successful
  record is CPU command-recording success, not proof of GPU execution or image quality.

## Renderer contract

1. Keep the signed loader, function table and Vulkan device alive throughout the
   context lifetime. Load from the trusted runtime directory using the existing
   probe's deployment arrangement. Enumerate on this device and pass the selected
   ID to `Context::open`. Never persist IDs across process/device/driver sessions.
   Check every result, including creation and destruction.
2. Identify the actual main scene depth, motion vectors, jitter, camera position,
   normalized world-space basis, near/far planes, vertical FOV and world-unit
   scale. Missing vectors require reconstruction work; a zero-vector texture or
   identity camera is not a substitute. Unjittered rendering may supply zero
   jitter only after that convention is verified. FOV is radians; elapsed time is
   milliseconds between **rendered frames**. Vector scale converts stored vectors
   to pixels and must account for direction and jitter conventions. Inverted and
   infinite depth are explicit creation flags.
3. Supply distinct, single-sample, non-aliased Vulkan images with sampled usage
   on inputs and storage usage on output. This first recorder accepts one layer,
   one mip, exact active dimensions, float RG motion vectors, and R32F/R16F/R16UNORM
   depth. Color/output formats are RGBA8 UNORM, BGRA8 UNORM, RGBA16F or RGB10A2 UNORM.
   Their format must match the created context. Verify device format support;
   metadata validation cannot check allocation usage, aliasing memory or samples.
4. Color and output are display-size. Depth is active-render-size. Motion vectors
   are active-render-size unless the display-resolution vector flag is set.
   Transition actual Vulkan inputs to compute-readable layouts and output to
   storage/general, with correct stage/access barriers and queue ownership,
   before `record`. Resource state fields describe actual state; they do not
   perform the caller's preceding transitions. Issue the required output barriers
   before copying/compositing/presenting. Validate backend final-state behavior
   under Vulkan validation before wiring into the renderer's state tracker.
5. The generation rectangle is the composited scene content region and must have
   the same aspect as the depth/vector content. This implementation conservatively
   rejects mismatched ratios. The current 720p-to-ultrawide stretch is therefore
   not an accepted temporal input path; resolve geometry/composition mapping
   before enabling FG for that path. A display-size or format change requires
   draining the GPU, closing and recreating the context.
6. Use one render thread and one ordered graphics/compute queue for this first
   integration. Finish submission and wait for completion before resource reuse
   or another frame's recording. Keep inputs alive through their consuming GPU
   work. This conservative bring-up contract is not a final low-latency scheduler.
   No resource ownership, fences or GPU waits are managed by the recorder.
7. On an SDK dispatch failure, discard the dedicated FG command buffer: it may
   contain partial commands. Do not record essential game rendering into this
   same buffer. Drain any earlier submitted work before closing/recreating.
   If a successful recording is abandoned, call `reset_history()` before reuse.
   Also reset after bypassing generation, pause/resume, camera cuts and menus.
8. Call `close()` only after all recorded work is completed or discarded. Check
   failure and keep the DLL/device/object alive to retry if destruction fails.
   The destructor is a cleanup fallback with the same GPU-idle precondition;
   it does not wait on a queue or report errors. Do not use it as synchronization.
9. Present the previous/current real frames and intermediate output at the proper
   cadence. On a reset/error/bypass, present real frames only. Keep real-frame and
   generated/presented-frame counters separate. Generated images must never drive
   game timers, input, animation or physics updates.

## Still required before enabling in-game

The main pass's temporal inputs and camera provenance remain unverified. The
existing capability probe does not provide those resources. The renderer owner
must supply them and add the new source to the GPU target after reviewing file
ownership; this checkpoint deliberately changes no shared renderer/build files.

HUD separation/composition, Vulkan synchronization, output allocation, pacing,
latency behavior, resize/minimize recovery, and shader/driver execution remain
unimplemented or unvalidated. The recorder currently uses the supplied final
color and has no HUD-less resource path; interpolating a HUD can produce artifacts.
Do not expose an enabled launcher option until those paths and clean bypass are
tested. AMD recommends its swapchain callback integration for production; the
manual recorder is a bounded bring-up option for the existing Vulkan presenter,
not a replacement for presentation scheduling.

FG may increase displayed frames without advancing simulation more often. It
does not resolve the separate native 60 FPS timing/parity work, and it is not
FSR 4 upscaling. Neither feature is enabled merely by shipping SDK DLLs.

## CPU validation

The test executable uses mock SDK function pointers and synthetic image handles.
It loads no DLL, touches no GPU, starts no game and changes no installed files.
It covers provider selection/fallback refusal, descriptor lifetimes, call order,
input rejection, reset sequences, partial failures and recreation. Build from
the repository root with a Windows LLVM-MinGW toolchain:

```powershell
clang++ -std=c++23 -O1 -Wall -Wextra -Werror -DNOMINMAX -DVK_NO_PROTOTYPES `
  -Igpu -isystem gpu/vendor/externals/ffx-api `
  -isystem gpu/vendor/externals/vulkan-headers/include `
  gpu/jgpu_frame_generation.cpp tests/frame_generation_tests.cpp `
  -static -o frame_generation_tests.exe
./frame_generation_tests.exe
```

Vendor includes are system includes to avoid treating the pinned SDK's existing
unknown warning-pragmas as project errors. Project warnings remain errors.
Passing these tests does not validate the real SDK, Vulkan execution or gameplay.
