# Development handoff

The exact state another session can continue from. Update this file at every stopping point.

## Where the work is

* Repository: `https://github.com/Dopam3an/JStarsReborn` (public)
* Baseline: `main` at the 0.4.0 release (launcher + compatibility runtime, verified on Windows)
* Current work: branch **`feature/online-np`**

## What this branch changed

| Area | File(s) | What it does now |
|---|---|---|
| Sockets | `runtime/jstars_net.c` (new) | Real host UDP sockets for `libSceNet`, with the console's 16-byte address layout, FreeBSD errno values, non-blocking emulation and `sceNetSocketAbort`; `libSceNetCtl` reports this host's address. **Virtual-port (P2P) multiplexing**, because the game uses socket type 6. Off unless `--online` is given. |
| NP sign-in | `runtime/jstars_platform.c` | A local identity and a real state callback, delivered from the game's own `sceNpCheckCallback` on its own thread with the guest ABI. |
| Probe | `runtime/jstars_loader.c`, `docs/EXPERIMENTS.md` | `--experiment online-probe`: unimplemented online imports log and return `0x80550006` instead of stopping the run, so one run maps the path. `--trace-net` adds bounded per-call tracing. |
| Analysis | `tools/re/map_imports.py`, `tools/re/function_calls.py` | Build the function map from the owner's own eboot. No game code or data is copied. |
| Documents | `docs/PORT_ARCHITECTURE.md`, `docs/RELEASE_CHECKLIST.md`, `docs/reverse-engineering/*` | Migration boundary, gates, function map, dependency matrix. |

## What is proven, and how

* Host self-test: **174 checks, 0 failed**, on Linux and on the owner's Windows machine
  (run `20261101-200324-…-e43adb`), including 27 socket checks and 13 NP checks. The socket checks cover a
  loopback datagram round trip and two virtual ports sharing one UDP port.
* `eboot.bin+0x2cc760` passes family 2, **type 6**, protocol 0: the game uses P2P sockets with virtual ports.
* `eboot.bin+0x2cc710` creates one socket, sets two options, binds it and starts a receive thread whose loop is
  `sceNetRecvfrom` → `sceNetErrnoLoc` → a handler at `+0x2b2410`.
* `eboot.bin+0x2bdd40` starts the online services in this order: `sceNetInit`, `sceNetPoolCreate`,
  `sceNetCtlInit`, `sceSslInit`, `sceHttpInit`, `sceNpSetNpTitleId`, `sceNpMatching2Initialize`,
  `sceNpSignalingInitialize`, `sceNpRegisterStateCallback`, `sceNpSetContentRestriction`.

## What is not proven

* No part of Matching2 or Signaling is implemented, so no room exists and no peer has ever been contacted.
* The virtual-port framing is this project's own. Two JStarsReborn instances understand each other; a PS4 does not.
* The NAT type the runtime reports is not measured.
* Human verdicts on controls and audio are still outstanding.

## Exact commands

```
make -C runtime jstars-boot && runtime/jstars-boot --selftest-host --log /tmp/st.log   # 174 checks
python3 -I tools/re/map_imports.py <game-folder> --out docs/reverse-engineering/FUNCTION_MAP.md
python3 -I tools/re/function_calls.py <game-folder> 0x2cc710 0x2bdd40
make -C runtime jstars-boot-gpu.exe GPU_LIB_WIN=<libjstars_gpu.a>                      # Windows runtime
tools/windows_verify/run-gpu.ps1 -ExeName <exe> -PadScript tests\pad-online-probe.txt \
    -ExtraArgs "--online --trace-net --experiment online-probe" -GpuSeconds 200
```

## The next step

Implement `libSceNpMatching2` far enough for one room, in this order, each with host tests first:

1. `sceNpMatching2CreateContext` / `RegisterContextCallback` / `ContextStart` / `GetServerId` /
   `GetWorldInfoList` — context lifecycle and the asynchronous request/callback shape. The game's request
   structures are not yet confirmed against the executable; confirm each one with `tools/re/function_calls.py`
   on the caller before implementing it.
2. `CreateJoinRoom`, `SearchRoom`, `JoinRoom`, `LeaveRoom`, room member events.
3. `libSceNpSignaling`: give the game each peer's address, which is what its own transport needs.

Keep the backend behind an interface: the first one can be a room directory on the local network, and a
self-hosted server can replace it without touching the library layer.
