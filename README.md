# JStarsReborn

An unofficial Windows x64 PC port runtime for **J-Stars Victory VS+** (PS4, CUSA01593 version 01.02). It runs the original
x86-64 game code from **your own, already-decrypted dump** natively, and replaces the console's system services, graphics
(a shadPS4-derived Vulkan renderer), audio (WASAPI) and input (XInput and keyboard). A launcher, `JStarsReborn.exe`,
checks and prepares the dump and starts the game.

No game files, keys, firmware or decryption tools are included or downloaded. The game folder is only ever read.

> **Legal:** unofficial fan project, not affiliated with or endorsed by the game's publisher, developer or rights holders.
> Use only a dump you made yourself from a copy of the game you legally own. Piracy is not supported in any form.
> Provided as is, without warranty. Read the full [DISCLAIMER.md](DISCLAIMER.md).

**State (0.5.2, pre-release):** on the owner's Windows 11 PC the game boots, shows the title, creates and loads its own
save data, and runs the menus and Free Battle matches, driven by scripted test input and by a live controller. Movies are
skipped (no decoder) and trophies are an empty set. Whether it plays well and sounds right has **not** been confirmed by a
human tester yet. See [CHANGELOG.md](CHANGELOG.md) for the full list of known issues, and
[docs/RELEASE_NOTES_0.5.2.md](docs/RELEASE_NOTES_0.5.2.md) for what changed after the 0.5.1 playtest.

> **60 fps (experimental):** the launcher's Settings page has a "60 fps" option. It sets the game's own engine rate to 60 and its fixed
> 30 Hz timing constants for 60, only after checking the game build. In a scripted 6-minute Free Battle the game held 60 fps and the battle
> timer ran in real time; some animations or effects may still be off. 120 fps is not offered (the game's timers cannot run exactly at
> 120); the plan is 60 fps plus frame generation. See [docs/reverse-engineering/NATIVE_60_EXPERIMENT.md](docs/reverse-engineering/NATIVE_60_EXPERIMENT.md).

> **Online play is still in testing.** It is peer-to-peer (no server) and off by default: turn it on in the launcher's
> **Online** page. A host can open its ports with UPnP and share a room code; players on the same network or with the code
> see the host's Friendly Match room (0.5.2 fixes the search crash testers hit). **Joining a room, and so an actual match between two
> players, does not work yet.** A server-based online stack (shadPS4's network code, [docs/ADR-008-online-shadnet.md](docs/ADR-008-online-shadnet.md))
> is in the source but switched off until a lobby server is settled.
> Nothing contacts PlayStation Network and no account is used. See [docs/ADR-007-online.md](docs/ADR-007-online.md).

## Using it (Windows)

1. Build the release (below), or use a release ZIP, and unzip it anywhere (paths with spaces are fine).
2. Start `JStarsReborn.exe`, choose your game folder (the one with `eboot.bin`, `sce_sys` and `sce_module`), press Play.
3. Close the game window to end a session. Settings, saves, prepared data and per-session logs live in
   `%LOCALAPPDATA%\JStarsReborn`.

Controls: an Xbox-compatible (XInput) controller, or the keyboard when no controller is connected (mapping in
[docs/INPUT.md](docs/INPUT.md) and on the launcher's Controls page).

## Layout

| Path | What it is |
|---|---|
| `src/launcher/` | the launcher (Win32): pages, session tracking, diagnostic report, preparation front end |
| `src/prepare/` | native preparation: SELF/ELF parsing, relocation, import binding, linking `libc.prx` and `libSceFios2.prx` into one boot image |
| `runtime/` | the execution runtime (C): loader, kernel/OS replacements, file system, save data, audio, input, GPU host glue |
| `gpu/` | the vendored shadPS4 renderer (pinned revision, local patch in `gpu/patches/`, see `gpu/VENDOR.md`) |
| `tools/` | build and release scripts, the Python reference preparation tools, Windows verification runners, `vkprobe` |
| `tests/` | launcher core tests, scripted pad inputs for automated runs, older foundation tests |
| `docs/` | architecture decisions (ADR-005, ADR-006), import audit, input, build and release instructions |
| `evidence/` | logs and records of the runs on the owner's hardware that the change log refers to |
| `src/app`, `src/core`, `src/setup`, ... | the 0.2.x DX12 foundation and setup tool (`JStarsFoundation` target), kept for history |

## Building

The release is cross-compiled on Linux with mingw-w64 (GCC 13, posix threads) plus clang-18 for four renderer units:
see [docs/BUILD_RELEASE.md](docs/BUILD_RELEASE.md). In short:

```
cmake -S gpu -B build-gpu-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/gpu/cmake/mingw-w64-x86_64.cmake
ninja -C build-gpu-win
tools/build_release.sh build-gpu-win/libjstars_gpu.a build/release
```

Checks that run on Linux: `tools/prepare_native_check.sh` (native preparation is byte-identical to the Python tools),
the launcher core tests, and the runtime host self-test (`runtime/jstars-boot --selftest-host`).

A native Windows **host-service test build** is also available through [tools/windows_build](tools/windows_build/README.md).
Its verified 266-check result is from `656e15c`; it is not the GPU game launcher. Linking the supplied GCC renderer with the
installed llvm-mingw C++ libraries remains unresolved.

## History

- 0.2.x: native DX12 foundation, setup tool and audit ([PROJECT_AUDIT.md](PROJECT_AUDIT.md)).
- 0.3.0 (build w): execution runtime plus Vulkan renderer; a whole scripted Free Battle on Windows hardware
  ([docs/checkpoints/README-CHECKPOINT-0.3.0.md](docs/checkpoints/README-CHECKPOINT-0.3.0.md), `evidence/windows-gpu-0.3.0/`).
- 0.4.0: launcher, native preparation, normal play sessions, renderer storage-format fix
  (`evidence/windows-launcher-0.4.0/`).
- 0.5.0 (pre-release): online in testing: real sockets, local sign-in answers, Matching2/Signaling, room hosting and search;
  launcher online setting.
- 0.5.1 (pre-release): peer-to-peer room codes over the internet, UPnP, launcher Online page.
- 0.5.2 (pre-release): fixes from the first public playtest (online room-search crash, audio feed priority, system error dialog) and
  an experimental 60 fps option ([release notes](docs/RELEASE_NOTES_0.5.2.md)).

## License

See also [DISCLAIMER.md](DISCLAIMER.md).


The runtime, renderer and launcher together are GPL-2.0-or-later (the vendored shadPS4 code is GPL-2.0-or-later); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`. J-Stars Victory VS+ and its characters belong to their
owners; this project is not affiliated with them.
