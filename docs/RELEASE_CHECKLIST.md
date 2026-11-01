# Release checklist

Four independent gates. A version can satisfy one and not the others; this file says which, with evidence.
Nothing is ticked without a test or a recorded run that is named here.

## A. Runtime playability (the compatibility build)

| Item | State | Evidence |
|---|---|---|
| Boots the supported build from the owner's dump | done | `evidence/windows-gpu-0.3.0/`, `evidence/windows-launcher-0.4.0/` |
| Title, menus, save creation and reload | done | sessions 20261101-144715, -150628 |
| A complete Free Battle | done | scripted: run 20261101-070533; with a controller: session 20261101-144715 |
| Launcher: folder validation, preparation, play, exit, diagnostics | done | `evidence/windows-launcher-0.4.0/README.md` |
| Human verdict on controls and audio | **pending** | a 90-minute session was played (20261101-170926, exit 0); the owner has not reported how it played or sounded |
| Keyboard play end to end | **not shown** | presses reach the runtime; the game was not seen to leave the title on one |
| Every mode, progression, movies | not done | movies have no decoder; modes beyond Free Battle and the J-Adventure map are untested |

## B. Online

| Item | State | Evidence |
|---|---|---|
| Opt-in network tracing and a Windows Online Battle probe | done | `--trace-net`, `--experiment online-probe`, `tests/pad/pad-online-probe.txt` |
| Sockets with the console's layouts, options and error codes | done | host self-test (27 checks), including virtual-port multiplexing |
| Which socket kind the game uses | answered | type 6 (P2P, virtual ports) at `eboot.bin+0x2cc760` |
| NP sign-in state and its callback | done | host self-test (13 checks); delivered from the game's own `sceNpCheckCallback` |
| Matching2 contexts, rooms, member events | **not started** | — |
| Signaling and reachable peer endpoints | **not started** | — |
| Two separate machines complete a Friendly Match, rematch, disconnect | **not started** | — |

## C. Features for 1.0

| Item | State |
|---|---|
| 60 FPS and 120 FPS with timing correctness and mixed-cap online evidence | not started; the recorded cadence is about 30 flips/s and its cause is not established |
| FSR 4 | not started; feasibility not yet investigated |
| Ultrawide (21:9, then 32:9) | not started |
| Reversible mod loading and texture replacement | not started |
| Persistent shader/pipeline cache | not started |
| 4 GiB buffer defect fixed with merged arenas and sparse binding accounted for | not started; traced only |

## D. Source-based port gates

Every one of these is **not met** today; the shipped build executes the original executable.

| Gate | State |
|---|---|
| Clean checkout plus documented toolchain builds the PC game code reproducibly | the runtime and launcher build reproducibly; there is no PC game code yet |
| The final path loads no original PS4 executable or module as code | not met (`docs/reverse-engineering/DEPENDENCY_MATRIX.md`) |
| Core logic and platform interfaces are maintainable buildable source | platform interfaces yes; game logic no |
| Rendering uses a PC path | not met; the console command path is vendored |
| Modes, saves, controls, sound, movies pass acceptance testing | not met |
| Online passes complete sessions between machines | not met |
| Mods reversible and conflict-checked | not met |
| 60/120 FPS evidence | not met |
| FSR 4 and ultrawide have an explicit disposition | not met |
| Supported configurations have measured evidence | one machine only |
| No game assets in distributed source or packages | met (`tools/build_release.sh` excludes them; checked per release) |
