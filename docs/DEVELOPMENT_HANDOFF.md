# Development handoff

The exact state another session can continue from. Update this file at every stopping point.

## Where the work is

* Repository: `https://github.com/Dopam3an/JStarsReborn` (only `main` / tag `0.4.0` are pushed; this branch is not)
* Baseline: `main` at the 0.4.0 release (launcher + compatibility runtime, verified on Windows)
* Current work: branch **`feature/online-np`** in the working tree; exports with hashes in `E:\JStarsWork\AI_HANDOFF\incoming`

## What this branch changed

| Area | File(s) | What it does now |
|---|---|---|
| Sockets | `runtime/jstars_net.c` (new) | Real host UDP sockets for `libSceNet`, with the console's 16-byte address layout, FreeBSD errno values, non-blocking emulation and `sceNetSocketAbort`; `libSceNetCtl` reports this host's address. **Virtual-port (P2P) multiplexing**, because the game uses socket type 6; closing the last P2P socket on a port releases the real port. Off unless `--online` is given. |
| NP sign-in and requests | `runtime/jstars_platform.c` | A local identity and a real state callback (delivered from the game's own `sceNpCheckCallback`), the request API, availability / Plus / account country and language answered locally, WebApi (the player's own empty block list; every other path NOT_SIGNED_IN), a session-local Score board. |
| Matching2 | `runtime/jstars_platform.c` | Context lifetime: Create/Destroy, context and room-event callbacks, Start (CONTEXT_STARTED from a library thread), Abort, Stop, GetServerId (one local server). One lock, callbacks called unlocked, a per-slot generation so a start pending on a destroyed context is never reported. **No world, room or request API yet.** |
| Signaling | `runtime/jstars_platform.c` | Context lifetime only (ids 1..8 round-robin). The game's handler is stored and never called. |
| Audit | `runtime/jstars_audit.c` | A row for every net/NP import; the 0.4.0 socket rows were stale and are corrected. |
| Probe / tracing | `runtime/jstars_loader.c`, `docs/EXPERIMENTS.md` | `--experiment online-probe`, `--trace-net`, `--trace-np`. |
| Analysis | `tools/re/map_imports.py`, `tools/re/function_calls.py` | Build the function map from the owner's own eboot. No game code or data is copied. |
| Documents | `docs/PORT_ARCHITECTURE.md`, `docs/RELEASE_CHECKLIST.md`, `docs/ADR-007-online.md`, `docs/reverse-engineering/*` | Migration boundary, gates, online design decision, function map, dependency matrix. |

## What is proven, and how

* Host self-test: **278 checks, 0 failed** on Linux at commit `711045b` (266 at `900f472`). On the owner's Windows PC: 257/0 with online-h
  (commit `19873a3`, run `20261102-005629-…-754a29`) and the online-i run below.
* Windows probe with online-h (`20261102-005629`): the game creates the Matching2 context (return address `eboot.bin+0x2bfce7`),
  registers both callbacks, then calls `sceNpSignalingCreateContext` (`+0x2c90e8`). 200 s, exit 7 (the requested limit), 6,035 flips,
  0 faults, 0 renderer errors; first-GNM stop exit 3 as before.
* Windows probe with online-i (`20261102-010101-…-2afd8e`): the game creates **two** signaling contexts, calls `ContextStart`, our
  library thread delivers CONTEXT_STARTED to the game's callback (`0x2002bfe50`) and it returns, the game calls `GetServerId` (`+0x2c02fe`) and
  then **`sceNpMatching2GetWorldInfoList`** (`+0x2c047a`, function `0x2c03e0`), the current gate. On leaving the menu it tears everything down in
  order: `SignalingDeleteContext` x2, `AbortContextStart`, `ContextStop`, callbacks cleared, `DestroyContext`.
* Windows probe with online-j (`20261102-010835-…-cef128`, commit `711045b`, self-test 278/0): `GetWorldInfoList` answers one local world;
  the game's callback (`0x2c04c0`) receives it, and the game **enters its Online Battle menus**: the captures show *Rank Match*, *Set Channel*
  (J-Customize, Area) and the character / stage select. No further online import was called in the 200 s run (the pad script only presses
  CROSS every 8 s, then CIRCLE); no fault, 5,648 flips at 187 s.
* `eboot.bin+0x2cc760` passes family 2, **type 6**, protocol 0: the game uses P2P sockets with virtual ports.
* `eboot.bin+0x2bdd40` starts the online services (`sceNetInit` … `sceNpSetContentRestriction`).

## What is not proven

* No world, room, member, request callback or peer connection exists. Multiplayer is **not** demonstrated.
* Matching2 / Signaling error values come from shadPS4's headers, not from the console.
* `AbortContextStart` on an already started context answers CONTEXT_NOT_STARTED; what the console answers is unknown (the game ignores it).
* The virtual-port framing is this project's own. Two JStarsReborn instances understand each other; a PS4 does not.
* The NAT type the runtime reports is not measured. Human verdicts on controls and audio are still outstanding.

## Exact commands

```
make -C runtime jstars-boot && runtime/jstars-boot --selftest-host --log /tmp/st.log   # 266 checks
make -C runtime jstars-boot-gpu.exe GPU_LIB_WIN=<libjstars_gpu.a>                      # Windows runtime
E:\JStarsWork\run-gpu29.cmd      # online-i probe: pad-online-probe.txt, 200 s, --online --trace-net --trace-np --experiment online-probe
python3 -I tools/re/map_imports.py <game-folder> --out docs/reverse-engineering/FUNCTION_MAP.md
python3 -I tools/re/function_calls.py <game-folder> 0x2c03e0
```

## Coordination

Codex (GPT) works on the same project through `E:\JStarsWork\AI_HANDOFF` on the owner's PC (`README.md`, `ACTIVE_WORK.md`,
`CLAUDE_TO_CODEX.md` / `CODEX_TO_CLAUDE.md`, dated `messages/`). Source exports go to `incoming/`; check `ACTIVE_WORK.md` before editing.

## The next step

1. A pad script that reaches the *Player Match* (friendly) path instead of *Rank Match*, using the online-j captures to map the menu,
   so the probe shows which room request the game makes first (`SearchRoom` 0x2c1460 or `CreateJoinRoom` 0x2c0bff).
2. `SearchRoom` (`0x2c1460`), `CreateJoinRoom` (`0x2c0bff`), `JoinRoom`, `LeaveRoom`, `SetRoomDataExternal`, `KickoutRoomMember`, room events,
   behind a backend interface whose first implementation is a LAN room directory.
3. Signaling `ActivateConnection` / `GetConnectionStatus` / `GetConnectionInfo` (peer address for the game's own P2P transport), then a
   two-instance test.
