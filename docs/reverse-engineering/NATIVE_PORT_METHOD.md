# Native port implementation method

## Direction

The project direction is a working native PC port, with targeted reverse
engineering for normal-speed 60+ FPS, graphics improvements and mod support.
A complete source-level decompilation is not a prerequisite. The existing native
x86-64 execution, host service implementations and Vulkan graphics translation
remain the foundation. Calling the original machine code natively does not mean
that its source has been recovered.

Requested features remain optional roster unlock, added roster entries,
support-character playability where viable, FSR upscaling/frame generation,
and online play. These need separate evidence and implementation; the native
architecture alone does not provide them.

## Installed Bloodborne package study

The locally supplied package identifies itself as bbport 1.9.7 in its file
manifest. Its README describes native execution of the original game code, not
a complete decompilation. The inspected timing scripts all match that manifest.
The package has prepared-image/patch tooling and binaries, but its inspected
tools/scripts/docs do not include a full runtime C++ source checkout or a Ghidra
project. Do not confuse this package with the newer public fork's C++ frame-hook
implementation discussed in `FRAME_TIMING.md`.

Its bundled scripts provide useful, concrete patterns:

| Relative file | Mechanism | What transfers to this project |
| --- | --- | --- |
| `scripts/patches.py` | Compiles selected version-specific XML changes into a separate patch file; fingerprints groups of original patch sites. | Keep the game dump unchanged. Gate modifications on build identity and expected code; validate patch overlaps. |
| `scripts/air_drag.py` | Uses the update routine's actual elapsed time to exponentiate a per-step drag factor. | Correct nonlinear damping separately from additive movement; do not replace every constant with a half-value. |
| `scripts/effect_time.py` | Accumulates measured elapsed time and advances effects only in whole 1/60-second units, keeping the remainder. | A subsystem can preserve a stable fixed step while presentation runs faster. Respect pause/zero-step requests. |
| `scripts/cloth_time.py` | Computes shared per-frame timing and limits sudden changes for a sensitive visual subsystem. | Classify subsystem stability needs. Its smoothing policy is not automatically appropriate for combat or network time. |
| `scripts/cloth.py` | Corrects integration, damping and constraints separately. | Higher FPS may expose several independent physics assumptions, even after the main timer is fixed. |
| `scripts/foliage_time.py` | Replaces frame-count-driven visual clocks; comments describe measured watchpoint tracing. | Find a value whose rate changes with FPS, identify its writer, then trace and patch only that path. |

These are observations from code and embedded development notes, not tests run
here of Bloodborne's gameplay. No bundled script was executed, no binary or game
data was copied into the repository, and no settings, saves or account data were
needed for this study. GPL source should retain its attribution and applicable
license if code is reused later; the current analysis tool is independent.

## Incremental reverse engineering workflow

1. Identify the exact game build and map imports, native wrappers and image-relative
   addresses. Use the timing candidate report as a starting list, not a function map
   proven by execution.
2. Observe candidate state during identical 30 FPS and 60 FPS battles. Measure elapsed
   real time, battle timer, movement distance, attack duration and update counts.
3. Use a debugger/watchpoint or narrowly scoped in-runtime instrumentation to find
   writers and consumers. Start with the timestamp/counter cluster documented in
   `FRAME_TIMING.md`; it is not yet confirmed as the simulation clock.
4. Disassemble and decompile only the reached routines in a private analysis project.
   Record prototypes, calling convention, object-field meanings, side effects,
   thread, confidence and evidence. Distinguish inferred names from recovered symbols.
5. Implement an optional, build-checked detour or replacement for the understood
   routine. Preserve registers, flags, stack alignment and guest ABI. Prefer host-owned
   storage/code allocation over assuming an unused region of the guest is safe.
6. Compare the same scene and actions against the unmodified 30 FPS baseline. Test
   60/90/120 FPS, stalls, scene transitions, menus, audio and combat. Keep networking
   on its original real-time semantics unless a separately validated design changes it.
7. Grow a documented native interface from validated functions. That interface can
   later expose character registration, selection policy and mod callbacks without
   requiring a complete rewrite of the game first.

## First milestone and alternatives

First milestone: normal-speed battle timer and movement at 60 real rendered FPS,
with an unchanged default and reproducible evidence. Find whether a guest rate mode
already couples presentation and tick size before writing a new clock.

If the engine supports elapsed-time updates, feed the correct per-frame duration
and audit remaining frame-dependent subsystems. If combat depends on integer game
frames, retain fixed 30 Hz simulation and interpolate visual state for higher-rate
rendering. This needs a real update/render separation; repeatedly presenting one
image does not satisfy the goal. Frame generation is a distinct optional feature.

Public reports contain only neutral experiment IDs, source references, addresses,
measurements and original implementation code. Private handoffs, local paths,
computer names, memory dumps and game binaries stay outside the repository.
