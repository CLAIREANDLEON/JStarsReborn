# Bloodborne source reference

Inspected 2026-11-03: [Supermedo/bloodborne_pc, windows](https://github.com/Supermedo/bloodborne_pc/tree/b48052617a919fc4d6b10ae3395e209c9e94e17a),
pinned at `b48052617a919fc4d6b10ae3395e209c9e94e17a`. This source tree supplements
the previously inspected packaged installation and the separate Visy fork.
Their implementations and revisions must not be treated as identical. This was
source inspection, not a fresh Bloodborne gameplay validation. No Bloodborne
source or game-specific machine code was copied into J-Stars by this change.

## Timing methods

| Inspected source | Mechanism | J-Stars application |
| --- | --- | --- |
| [air_drag.py](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/scripts/air_drag.py) | Converts a per-step damping factor to an exponential function of elapsed time; guards original instructions. | Identify real movement/damping consumers before adjusting coefficients. A correct countdown alone does not establish jump-distance parity. |
| [effect_time.py](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/scripts/effect_time.py) | Accumulates elapsed time, passes whole multiples of 1/60 second to effects and retains the fraction; honors zero-time frames. | A possible method for subsystems that cannot handle arbitrarily small steps. Preserve pause semantics. It is not evidence that J-Stars effects need a 60 Hz quantum. |
| [cloth_time.py](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/scripts/cloth_time.py) | Smooths visual cloth time across frames, bounds it, and shares the result across cloth instances. | Consider only after identifying a comparable visual instability. Do not smooth authoritative combat or network time using this policy. |
| [foliage_time.py](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/scripts/foliage_time.py) | Replaces traced per-frame wind-clock increments with an appropriate time step; its comments document watchpoint and optical-flow measurements. | Trace the writer and classify the object, then compare the same scene at 30/60. Do not replace shared constants globally. |

J-Stars has its own rate getters and 300-unit integer clocks. Native 60 remains
the controlled target. A later higher-rate implementation must preserve fractional
integer-clock progress or separate simulation from rendering; changing the integer
rate to 120 alone loses time. See [native 60 evidence](NATIVE_60_EXPERIMENT.md).

## Temporal rendering methods

The reference's [frame analysis](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/docs/upscaler.md)
maps scene depth, camera constants and the UI boundary before integration. Its
Bloodborne-specific render targets, matrix offsets and shader signatures cannot
identify J-Stars resources. The applicable sequence is capture, classify,
validate reprojection, then integrate the effect.

The [frame-generation interface](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/gpu/shadps4/video_core/renderer_vulkan/vk_frame_generation.h)
captures depth/motion at the temporal-upscaler boundary, captures color before
HUD composition, and supplies completed frames to presentation. Its interface
separates simulation frames from generated output, handles missing captures and
tracks GPU completion for resource reuse. J-Stars currently has capability
discovery only; it still needs verified depth, motion, jitter and UI inputs.

The [motion-vector investigation](https://github.com/Supermedo/bloodborne_pc/blob/b48052617a919fc4d6b10ae3395e209c9e94e17a/docs/motion_vectors.md)
shows why camera motion alone is insufficient for animated objects. It discusses
draw-history matching, topology changes, missed frames, memory budgets, barriers
and visual validation. J-Stars needs its own motion classification and measurements;
successful shader compilation cannot establish image quality.

## Integration boundaries

Keep source dependencies pinned, preserve notices, and inspect each reused file's
license and transitive dependencies. The repository-level GPL-2.0 label does not
establish that every component has the same license. Keep the original dump
unchanged, generate patches privately, reject unknown executable identities, and
publish only source, synthetic tests and sanitized findings. Native execution
does not require recovering every original function before useful port work.
