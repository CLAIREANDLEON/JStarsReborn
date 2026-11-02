# Native high-frame-rate investigation

Checkpoint: 2026-11-02, source base `88dcdb3`. This is research, not a working
60 FPS gameplay patch. Default gameplay remains unchanged.

## What is established

The delayed flip experiment reached a battle: the timer advanced 25 seconds in
25.209 real seconds at about 30 FPS, then 20 seconds in 10.047 real seconds at
about 60 FPS. This proves frame-dependent battle timing. It does not prove that
every engine subsystem shares one clock. See `../GRAPHICS.md` for the experiment.

The objective is real rendered 60 FPS and higher at normal gameplay speed.
FSR frame generation remains a separate presentation feature, not a substitute
for repairing native simulation timing.

## Primary-source precedents

These are native execution ports and patch projects, not evidence that the games
have been fully decompiled. References were inspected on the checkpoint date.

- [Bloodborne Windows port frame hooks](https://github.com/visy/bloodborne_pc_windows_port/blob/f103da8fe5bacaf9e36af98cdab94850277ac821/gpu/shim/bbport_frame_rate.cpp)
  and [timing rules](https://github.com/visy/bloodborne_pc_windows_port/blob/f103da8fe5bacaf9e36af98cdab94850277ac821/gpu/shim/bbport_frame_rate_logic.h):
  changes the game pacer and feeds measured elapsed time into the game update.
  Separately adjusts frame-based increments and damping. Some effects retain a
  fixed 60 Hz step through an accumulator. Hooks check expected instructions and
  redirect selected constant operands into per-frame storage. These addresses
  and engine-specific policies cannot be transplanted into J-Stars.
- [Bloodborne timing fix investigation](https://github.com/yogesh1239/BB-Sprint-Fix-Patch/blob/main/docs/TIMING_FIX.md):
  compares clocks across frame rates and uses hardware watchpoints to identify
  writers. It distinguishes update steps from frame-to-seconds duration constants
  and mathematical coefficients. The author labels this experimental and notes
  incomplete high-FPS behavior; it is a method reference, not a universal fix.
- [The Last Guardian timing issue](https://github.com/HayatoG/tlg_pc/issues/5):
  identifies flip-rate and time-quantization sites, with a 60 FPS patch marked
  implemented but physics, animation and cutscene validation still open. This
  illustrates why removing a limiter and checking simulation are separate tasks.
- [Gravity Rush 2 native-port issue](https://github.com/HayatoG/gr2_pc/issues/2)
  and [patch loader](https://github.com/HayatoG/gr2_pc/blob/a2e69a1bc506865716cb4e16f2c50f047bf65e57/port/scripts/gr2_patches.py):
  high-frame-rate work remains open; that loader explicitly has no built-in GR2
  patches. Do not present this port as a proven unlocked-framerate solution.
- [inFAMOUS Second Son status](https://github.com/HayatoG/iss_pc/blob/37c05ba21bb328f5962edfbc42bb5360f7db42ed/README.md):
  reports uncapped rendering but only partial 60 Hz support and planned higher
  rates. It is a useful native-port reference, not proof that J-Stars needs only
  the same presentation settings.

## J-Stars static leads

Input: CUSA01593 01.02, eboot SHA256
`d43b7b5db10b5808c06691b27ef0ce1b5d27da6b1e07d811db5bea125ceaed54`.
All addresses below are image-relative, not host addresses.

The read-only `tools/re/timing_candidates.py` found 10 timing-related import stubs
and 67 float constant locations with candidate references. It includes tail
jumps that a direct-call-only map missed. Linear disassembly can misinterpret
embedded data and misses indirect dispatch; every candidate needs control-flow
and live-hit validation. Counts are reconnaissance, not recovered functions.

| Lead | Static observation | Next check |
| --- | --- | --- |
| `0x4043f0` | Tail wrapper to process-time import `0x539d30`; callers include `0x433e84`. | Measure call frequency and identify the surrounding object's lifetime. |
| `0x433e76` / `0x433e89` | Copies the previous timestamp and stores a new process timestamp in object fields `+0x5d8` / `+0x5e0`. | Determine whether consumers use their difference for profiling or simulation. |
| `0x433f6e` onward | Increments object counter `+0x5c0`; adds an integer to `+0x5c4`; publishes globals at `0x81f370`, `0x81f374`, `0x81f378`. | Measure increment and cumulative-counter rates before/after flip override. Do not yet label them simulation ticks. |
| `0x376850` | Reads global `0x81f374` near entry and uses it in a stateful routine. | Follow consumers and correlate with gameplay updates. |
| `0x43d93a` | Calls SetFlipRate. Surrounding routine maps input mode 2 to rate 1 and mode 3 to rate 2; other modes use rate 0. | Establish whether the mode also controls game update increments; changing only the host override bypasses any such guest policy. |
| `0x475390` | Tail wrapper to submit-and-flip import; direct callers at `0x480648`, `0x480a58`, `0x481e25`. | Correlate submission with the main update and timing counters. |
| `0x4a2ca0` / `0x4a2cb0` | Wrappers to TSC and frequency imports; callers in `0x4a15xx` / `0x4c3cxx`. | Classify middleware/profiling use before treating these as gameplay clocks. |
| `0x2fa15c` | Multiplies by a 1/30 float and accumulates into an object field. | Determine the meaning of the input scale and the object's subsystem. |

The 33.33-value cluster at `0xcbe82` through `0xcc295` contains threshold loops
and calls on object arrays. Inspection did not establish a simulation clock.
This is a concrete reason not to globally replace all 30 Hz-looking constants.

## Next implementation decision

First instrument the timestamp/counter cluster and a battle-timer writer in the
same controlled 30-to-60 run. Log image-relative sites, elapsed time, update counts
and candidate values. No unrestricted memory dumps should enter public reports.

If the engine accepts elapsed time, correct the central update value first, then
audit animations, hit windows, projectiles, AI, camera, effects and menu timers.
For an additive per-30-Hz-frame step, scale by `30 * dt`; multiplicative damping
needs `factor ** (30 * dt)` rather than the same linear scaling. Preserve constants
that describe durations rather than per-update changes.

If combat is fundamentally integer-frame based, keep simulation at 30 Hz using an
accumulator and render interpolated states at 60/90/120+ Hz. This requires finding
separate update/render boundaries and retaining previous/current visual states;
duplicating the same presented image is not native smooth rendering. Do not skip
whole game iterations until input, audio and network responsibilities are understood.

Any eventual patch must be optional, reversible in memory, gated by executable
identity and expected original instructions, and tested at 30, 60, 90 and 120 FPS
plus stalls and scene transitions. Keep OS clocks, audio clocks and network clocks
at real speed. A global host-clock multiplier would hide symptoms elsewhere.

## Reproduction and stopping point

Requires Python and Capstone. From the source root, with placeholder paths:

```text
python tools/re/timing_candidates.py <owned-game-folder> --out <private-report.json> --address 0x81f370 --address 0x81f374 --address 0x81f378
```

Executed on the identified dump; selected leads inspected locally. The tool
writes addresses, operation names and counts only, without input paths or game
bytes. No timing mutation, new GPU build, or new in-game run was performed during
this research pass. Resume with live observation of the listed leads.
