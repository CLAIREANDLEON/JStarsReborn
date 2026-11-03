# Native 60 FPS candidate

Tested on Windows on 2026-11-02. The engine-rate candidate fixes the measured
battle-countdown speed at approximately 60 real rendered FPS and completed one
scripted Free Battle through its results/rematch screen. This is an opt-in
experiment, **not a complete validation of movement, combat, physics or effects**.
The normal launcher and its 30 FPS default are unchanged.

## What changed

The presentation-only override left the engine's configured rate at 30. A first
guest-side experiment changed presentation mode from 2 to 1, also changing its
returned counter step from 2 to 1. That still produced a double-speed battle
timer. The presentation counter is not a sufficient simulation-clock fix.

A hardware write watchpoint on the live countdown identified its writer at
image-relative `0xb0dd2`. The countdown is computed from a frame limit and elapsed
integer frames; `0xb1004` and `0xb100b` advance the elapsed counters. Its conversion
to seconds calls `0x404360`.

Following that getter established the game's configured update rate:

| RVA | Role |
| --- | --- |
| `0x1e01b1` | Initializes the engine rate to 30 before the main loop. |
| `0x404330` | Stores integer and float representations of the configured rate. |
| `0x404350` | Float rate getter; 83 static direct-call references found. |
| `0x404360` | Integer rate getter; 61 static direct-call references found. |
| `0x7a88c0` / `0x7a88c4` | Float / integer rate storage. |
| `0x433e6a` | Passes the presentation mode into the game's virtual presentation function. |

Several inspected float-getter callers divide step values by this rate, and
others multiply durations by it. This supports testing the engine setting before
individually changing constants. Reference counts are static observations, not
proof that every subsystem uses the setting.

The second candidate changes the initializer to 60 and requests presentation
mode 1 from startup. Starting afresh matters: a 300-second battle initializes
18,000 elapsed-frame units instead of 9,000. Changing the setting in the middle
of a match would reinterpret existing frame counts.

No host clock, audio clock, network clock, shader or renderer implementation is
modified. This is fixed-rate 60 FPS work; arbitrary unlocked rates are not yet
implemented or tested.

## Measured result

Private run identifier: `native-rate60-20261103-031438` (UTC start). The run used
the existing scripted Free Battle input, muted audio output and a fresh test-save
directory. It ran to the configured 420-second stop, exit 7, without a fault.
Captures showed combat, character changes and the results/rematch screen.

An uninterrupted interval from the read-only observer:

| Measurement | Start | End | Difference |
| --- | ---: | ---: | ---: |
| Observer wall time, seconds | 10 | 55 | 45 |
| Guest frame counter | 11,191 | 13,880 | 2,689 |
| Elapsed battle frames | 132 | 2,821 | 2,689 |
| Displayed remaining seconds | 297 | 252 | 45 |

At the verified internal rate of 60, 2,689 battle frames represent 44.8167 seconds
in 45 real seconds: **0.99593x speed**, with **59.756 real frames/s**. Subsequent
samples included a stopped battle clock and are not included in this uninterrupted
rate calculation. This does not establish that attacks, movement or effects all
run at their original speed.

## Reproduce without changing the installed game

Supported identities:

- CUSA01593 01.02 eboot SHA256:
  `d43b7b5db10b5808c06691b27ef0ce1b5d27da6b1e07d811db5bea125ceaed54`
- Original prepared boot SHA256:
  `8d356ebed54bdc6f985a9c88bbf48af370b6941aa9fe6225bf374d96411ef652`
- Tested runtime SHA256:
  `e185b77c93b0ed5ed76d56c5cc3f306087b0eb7d5f146f76484a3c5811c1a181`
- Generated 60 FPS candidate boot SHA256:
  `192ce218c49ec6c28aab9a0b6881eb6b95ac24968da3c9d8d5fdc3cd9a4104cb`

From the source root, substitute local paths:

```powershell
./tools/run_native60_experiment.ps1 -Runtime <runtime-exe> -Boot <prepared-boot> -Game <owned-game-folder> -WorkDir <private-test-folder> -Python <python-exe>
```

The launcher defaults to volume 0, creates a separate boot copy and test-save
directory, and verifies all supported input hashes. `-PrepareOnly` checks and
generates the candidate without starting the game. `-Volume 50` enables sound
when wanted; `-MaxSeconds 420` gives a bounded diagnostic run. Without a time
limit, close the game window to finish the test.

`tools/re/prepare_timing_experiment.py` also validates original instructions,
executable-page padding and relocation overlap. It refuses an existing output.
Six synthetic patch tests pass. The launcher preparation path reproduces the
same candidate hash as the tested run.

`tools/re/observe_battle_timing.py` can observe a selected test PID on Windows:

```text
python tools/re/observe_battle_timing.py --pid <test-pid> --seconds 60 --interval 5 --out <new-scalar-report.jsonl>
```

It validates the rate getter and countdown vtable and reports counters only.
Raw runtime logs and generated boot files remain private. Do not commit or
redistribute the generated boot image, which contains game data.

## Remaining validation

The follow-up static rate-consumer audit found integer clocks that advance by
`300 / engine_rate` units per update, using truncated integer division. At 60 Hz
this is exactly five units per update. At 120 Hz it is two, giving 240 units per
second instead of 300: these clocks would run 20% slow at sustained 120 updates/s.
Rates dividing 300 avoid this particular truncation; that alone does not make
them supported. The experiment deliberately accepts only 30 and 60.

The highest-priority unclassified fixed-step lead is `0x2f8d70`: the multiply at
`0x2fa15c` uses `1/30`, accumulating into object field `+0x160` and clamping to
`+0x34`. Its playback-like structure does not establish that it controls character
animation. Capture a live object and callers before patching it. Fade/blend leads
at `0x255120` / `0x255180` and `0x1ee120` / `0x1ee180` also use fixed 30 Hz
scaling. Authored animation sample rates and unrelated constants must be preserved.

See [the pinned Bloodborne source comparison](BLOODBORNE_REFERENCE.md) for
subsystem timing and temporal-rendering methods, and their limits.

### Follow-up live trace, 2026-11-03

A second muted native60 run reached Free Battle. A one-hit hardware execution
breakpoint at `0x2fa15c` captured input scale `xmm0 = 1.0`, object vtable RVA
`0x7b0f30`, clock zero, duration 0.25, and active flag 1. The captured stack
included return sites `0x43c67` and `0x79b0c`, consistent with the static caller.
Thus the fixed-step path executes in battle and this invocation was not supplied
half-speed input. This does not establish the object's update frequency or prove
that it governs character animation. No new gameplay patch was applied.

The caller obtains its scale through `0x3c2d0` / `0x8a640`; the latter returns
object field `+0x10` when its pause-like field `+0xc` is zero, otherwise zero.
A later 35-second read-only sample of the captured object found its clock
unchanged while guest frames continued. That inactive interval cannot measure
playback speed. Next: sample immediately on an active transition, establish the
owning subsystem, and compare its duration at 30 and 60 FPS.

The battle observer completed its 45-second report with the repaired deadline
handling. Its uninterrupted 10-to-45-second interval advanced 2,098 battle and
guest frames over 35 real seconds: 34.9667 game seconds at rate60, approximately
0.99905x. Earlier samples included stopped battle time and are excluded. Debugger
attachment can perturb timing, so this remains diagnostic evidence, not a
performance benchmark or full gameplay-parity result.

Compare movement distance, jump duration, attack/recovery timing, hit windows,
projectiles, AI, camera and effects against 30 FPS. Repeat with multiple
characters/stages, pause/resume, low-framerate stalls, cutscenes and match
transitions. Validate input responsiveness and sound separately. The completed
scripted match establishes basic stability and timer behavior, not full parity.
FSR frame generation, arbitrary unlocked rates and online determinism are
separate unfinished work.
