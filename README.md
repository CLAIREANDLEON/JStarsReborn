# JStarsReborn

An unofficial Windows x64 PC port runtime for **J-Stars Victory VS+** (PS4, CUSA01593 version 01.02). It runs the original
x86-64 game code from **your own, already-decrypted dump** natively, and replaces the console's system services, graphics
(a shadPS4-derived Vulkan renderer), audio (WASAPI) and input (XInput and keyboard). A launcher, `JStarsReborn.exe`,
checks and prepares the dump and starts the game.

No game files, keys, firmware or decryption tools are included or downloaded. The game folder is only ever read.

**State (0.4.0):** on the owner's Windows 11 PC the game boots, shows the title, creates and loads its own
save data, and runs the menus and Free Battle matches, driven by scripted test input and by a live controller. Movies are
skipped (no decoder), trophies are an empty set, and online features are not available. Whether it plays well and sounds
right has **not** been confirmed by a human tester yet. See [CHANGELOG.md](CHANGELOG.md) for the full list of known issues.

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

## History

- 0.2.x: native DX12 foundation, setup tool and audit ([PROJECT_AUDIT.md](PROJECT_AUDIT.md)).
- 0.3.0 (build w): execution runtime plus Vulkan renderer; a whole scripted Free Battle on Windows hardware
  ([docs/checkpoints/README-CHECKPOINT-0.3.0.md](docs/checkpoints/README-CHECKPOINT-0.3.0.md), `evidence/windows-gpu-0.3.0/`).
- 0.4.0: launcher, native preparation, normal play sessions, renderer storage-format fix
  (`evidence/windows-launcher-0.4.0/`).

## License

The runtime, renderer and launcher together are GPL-2.0-or-later (the vendored shadPS4 code is GPL-2.0-or-later); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`. J-Stars Victory VS+ and its characters belong to their
owners; this project is not affiliated with them.
