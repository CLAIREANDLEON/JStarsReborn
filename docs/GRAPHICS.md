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

No full GPU execution of this repair is claimed yet. The locally available
fps-a binary precedes the FFX probe; the delivered renderer library also predates
it. Obtain the matching renderer/build artifacts before the next game test.

## Existing measurements and remaining work

Claude reports about 30 FPS baseline and 60 FPS with immediate flip-rate 0,
but the faster run did not reach a usable battle-timer comparison. The E2 folder
`20261102-152027-OBLITERATOR-8a7dec` exists and its game log ends with player close
around 150 seconds, so it is not a completed 380-second experiment.

FSR capability discovery at `61d221f` is build-verified by Claude, not yet
Windows-runtime-verified. Temporal depth, motion vectors, jitter, exposure and
UI separation still need resource identification and capture evidence.
