# Graphics experiments

These switches are diagnostic. Higher submitted FPS is not evidence of correct
game speed, and the FidelityFX probe does not dispatch upscaling or frame generation.

## Delayed flip-rate experiment

`--frame-stats --flip-rate 0 --flip-rate-after 270` keeps the game's requested
rate until the monitor reaches second 270. The next guest submission applies
the override using a handle from a successful game SetFlipRate call. The monitor
only marks the threshold due; it does not call a guest-ABI renderer function.
Shared timing/handle state is guarded by the timing mutex. A failed delayed
attempt is logged, is not marked active, and is not retried every frame.

This enables a battle to start with the known 30 FPS input script before changing
the rate. Compare battle timer progression against wall-clock captures on both
sides of the actual logged switch. Do not infer gameplay correctness from FPS.
The saved handle model currently targets the observed single game video output;
close/reopen and multiple video ports require additional lifecycle coverage.

## Verification of the delayed-switch repair

Base: `61d221f`, incorporating and repairing Claude's untested delayed-switch patch.
Native Windows host build: 308 checks, 0 failed, with local UDP loopback enabled.
The host-only tests do not execute GPU wrappers. `tests/gpu_timing_tests.c`
separately exercises the actual GPU-enabled wrappers with mock backend functions:
threshold boundary, monitor versus submission thread, successful handle capture,
one-shot application, failure handling, no override, and immediate override.
Unrelated renderer dependencies abort if unexpectedly called.

From the repository root with llvm-mingw clang on PATH:

```powershell
clang -std=gnu2x -O2 -I gpu tests/gpu_timing_tests.c -static -lpthread -o gpu-timing-tests.exe
./gpu-timing-tests.exe
```

The repair was subsequently cross-built by Claude as `74b6700` and executed on
Windows with the matching `61d221f` renderer. Both the native host build and the
delivered GPU executable passed all 308 host checks. See the battle measurement
below; the override is a diagnostic and is not a correct gameplay-speed unlock.

## Existing measurements and remaining work

Claude reports about 30 FPS baseline and 60 FPS with immediate flip-rate 0,
but the faster run did not reach a usable battle-timer comparison. The E2 folder
`20261102-152027-PC-8a7dec` exists and its game log ends with player close
around 150 seconds, so it is not a completed 380-second experiment.

### Delayed-switch battle measurement, 2026-11-02

Run `20261102-153445-PC-893a67` used `pad-free-battle6.txt`,
`--frame-stats --flip-rate 0 --flip-rate-after 270 --ffx-probe`, with captures
starting at flip 6300 every 150 flips. It reached a battle before the switch.
At log time 274330.740 ms the guest submission applied rate 0 successfully.
The following complete sample reported 60.00 FPS versus about 30.20 before.

Timer values were read directly from the captured images; timestamps below are
the local capture timestamps embedded by the renderer in their filenames.

| Interval | Capture start (timer) | Capture end (timer) | Timer elapsed / real elapsed |
| --- | --- | --- | --- |
| Before switch | `frame_20261102_103846_669_game_000004.png` (258) | `frame_20261102_103911_878_game_000009.png` (233) | 25 / 25.209 s = 0.992x |
| After switch | `frame_20261102_103940_067_game_000018.png` (196) | `frame_20261102_103950_114_game_000022.png` (176) | 20 / 10.047 s = 1.991x |

The battle timer runs approximately twice as fast at the higher rate. Keep the
default game rate unchanged. A correct native unlock needs investigation of the
simulation timestep and presentation interpolation; frame generation is another
path to smoother presentation while preserving the game's 30 Hz simulation.
This measurement proves the timer speed problem, not every subsystem's timing.

## Windows FidelityFX probe

Runtime `74b6700`, SHA256
`e185b77c93b0ed5ed76d56c5cc3f306087b0eb7d5f146f76484a3c5811c1a181`,
successfully queried the signed SDK 2.4.0 Vulkan loader on Windows on 2026-11-02.
Run `20261102-153445-PC-893a67`, `game-gpu.log`, records:

| Effect | Versions returned, in provider order |
| --- | --- |
| Upscaling | 4.1.2, 3.1.6, 2.3.4 |
| Frame generation | 4.0.2, 3.1.6 |

Quality ratios: Native AA 1.00, Quality 1.50, Balanced 1.70, Performance 2.00.
The machine has an RX 9070 XT (driver 32.0.32015.7029); preserve the run's
device-selection log when comparing machines. Provider enumeration does not
prove context creation, dispatch, image quality, or frame pacing. In particular,
the first frame-generation provider is not FSR 3: a future FSR 3 mode must select
the enumerated 3.1.6 version explicitly rather than assume the default.

## Temporal input audit (source evidence, not captured resource identification)

The existing presenter receives the game's final video-output surface. Its
`PrepareFrame` in `gpu/vendor/shadps4/src/video_core/renderer_vulkan/vk_presenter.cpp`
finds that texture by video-output address, captures it before host scaling,
then calls the spatial `fsr_pass.Render`. This is a useful insertion boundary,
but does not by itself supply temporal inputs or a HUD-free scene.

| Input | Current evidence | Work required before dispatch |
| --- | --- | --- |
| Scene color | Presenter has the final output texture, dimensions and pixel format. | Identify a scene-only source and verify transfer function and exposure. |
| Depth | `Rasterizer::PrepareRenderState` in `vk_rasterizer.cpp` resolves the active depth attachment into `db_desc`. | Identify the main-camera depth across passes; verify size, inversion, near/far planes and lifetime at presentation. The last bound depth may be a shadow or other pass. |
| Motion vectors | No game resource identified. | Inspect shaders/attachments across consecutive battle frames; establish direction, scale, jitter convention and moving-object coverage. Camera-only reconstruction cannot represent character motion. |
| Camera jitter | No game projection constant or sequence identified. | Locate projection updates and verify the exact subpixel offset used by the scene. |
| Exposure | No exposure texture or pre-exposure value identified. | Establish color convention; evaluate the SDK's auto-exposure option against captured scene data. |
| HUD/UI | Current screenshot captures guest output before host passes, including any guest-composited UI. | Identify guest UI draws or capture scene before UI; do not confuse host overlay removal with game HUD removal. |
| Frame metadata | Host measures submission intervals. | Supply per-frame elapsed milliseconds, camera-cut/history reset, camera parameters and monotonic frame IDs; the five-second FPS summary is not a per-frame dispatch input. |

The required fields are declared in the vendored SDK's `ffxDispatchDescUpscale`
and `ffxDispatchDescFrameGenerationPrepare`. Next capture should record attachment
addresses, formats, extents, shader identities and draw order for a stationary
camera, camera movement, character movement, a scene cut and UI-only screens.
Do not infer usable motion vectors from a two-channel texture's format alone.
