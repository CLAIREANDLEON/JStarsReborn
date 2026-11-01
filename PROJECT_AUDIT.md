# JStarsReborn — Project Audit (checkpoint 0.2.1)

Date: 2026-10-31. Scope: the complete `JStarsReborn-0.2-setup.zip` (69 files) and the
development directive. This audit records only what was **executed and observed**. Where
something could not be executed, it says so.

## 1. Bottom line

JStarsReborn is a well-built **native Windows application shell**: a DX12 clear-screen
window, XInput polling, an XAudio2 test tone, validated settings, JSON logging and a setup
menu that installs that shell. It contains **no code that loads, translates or executes any
part of J-Stars Victory VS+**, no asset importer and no gameplay. The project's own
documentation already says this accurately, and this audit confirms it.

The gap to a playable port is not a list of bugs. It is the entire game-execution stack,
which is multi-person-year work, and it is gated on inputs that are not available in this
project or this session: an already-decrypted game dump, test hardware, and an
architecture decision only the owner can make (section 5).

## 2. Audit environment and its limits

| Available | Not available |
| --- | --- |
| Linux x86-64 container, GCC 13 MinGW-w64 cross compiler, Wine 9.0 (64-bit), native GCC 13 with ASan/UBSan, Python 3.13 | Windows host, GPU or D3D12 device, controllers or audio output, any J-Stars game file, PS4 reference captures |

Consequences: DX12 rendering, XAudio2 playback, XInput devices and the setup **GUI** were
not run in this audit. Their status rests on the previous session's Windows 11 / RX 9070 XT
evidence (`docs/TEST_REPORT.md`), which this audit did not reproduce. A compile check with the
project's pinned LLVM-MinGW toolchain was attempted, but the sandbox refused to execute that
downloaded toolchain. All builds below use GCC/MinGW. Wine is a reimplementation of
Win32, not Windows: a pass under Wine is evidence, not proof, of Windows behaviour.

## 3. Verified findings

### 3.1 Package integrity
- All 68 files listed in `checkpoint-manifest.json` matched their recorded SHA-256 values. None was missing.
- PE inspection of `JStarsReborn.exe` and `JStarsSetup.exe`: x86-64 GUI images importing only
  OS, CRT, D3D12/DXGI, XInput, XAudio2, COMCTL32, GDI32, SHELL32, OLE32 and BCrypt. There are
  **no networking imports** (no ws2_32/wininet/winhttp) and no emulator or VM dependency.
- Source review: process launch uses `CreateProcessW` with an explicit application name and a
  quoted command line. Reports open via `ShellExecuteW` on a generated JSON path. Installation is
  staged, hash-checked and committed by an atomic rename, never replacing an existing folder.
  No persistence, registry writes, elevation or telemetry were found.

### 3.2 Independent rebuild (second compiler)
The project had only ever been built with Clang + libc++. Building with GCC 13 + libstdc++
exposed:

| # | Defect | Severity | Fix |
| --- | --- | --- | --- |
| D1 | `#include <Xinput.h>` fails on case-sensitive filesystems, so no Linux/CI cross build was possible | Build blocker for cross builds | `<xinput.h>` (works with the Windows SDK and MinGW) |
| D2 | Four `-Wmisleading-indentation` warnings (`if(..)x;y;` on one line). Behaviour was correct | Hygiene | Split statements; GCC build is now warning-free |
| D3 | Tests assumed `temp_directory_path()` has no trailing separator (libc++ behaviour). libstdc++ keeps it, so two suites failed | Test portability | Normalize the temp base in the tests |
| D4 | The symlink-rejection test treated a reported-success `CreateSymbolicLinkW` as proof a link exists. Wine reports success without creating one, which gave a false failure | Test validity | Verify the reparse point exists, otherwise SKIP honestly |

**Coverage gap confirmed (not fixed):** the reparse-point and symlink rejection paths in
inventory/PKG intake have **never been exercised against a real link on any host**. The previous
Windows run skipped them (error 1314: no symlink privilege), and Wine cannot create them. To close
this, rerun `setup_inventory_tests` on Windows with Developer Mode enabled.

### 3.3 Test results in this audit (GCC 13 MinGW build, run under Wine 9.0)

| Suite | Result | Notes |
| --- | --- | --- |
| core | PASS, 8,336 checks | Matches the previous Windows result exactly |
| input | PASS | Normalization only, with no device present |
| setup_inventory | PASS, 32 checks, 1 skip | Skip = symlink fixture (D4) |
| setup_install | PASS, 126 checks | The previous Windows report listed 121 |
| pkg_intake | PASS, 24 checks | Synthetic headers only |
| asset_audit (Python, native Linux) | 13 PASS, 1 skip | |
| **gamecheck (new)** | PASS, 526 checks native under ASan/UBSan; 525 + 1 skip under Wine | Includes 50,000-input mutation fuzzing of the PARAM.SFO parser |
| Runtime `--fail-init` error path | Exit 1, structured `fatal` log entry | The window/D3D12 path cannot run here |
| **Crash reporter (new)** | Access-violation and `std::terminate` paths both produce valid JSON + `MDMP` minidumps; the next launch logs the earlier crash | Verified under Wine only |

Installer integration: after the documentation edits, `setup-payload.txt` was regenerated with
`tools/make_setup_payload.py`. The GCC-built `JStarsSetup.exe --install` then installed all 20
payload files with hash verification and a receipt, inside Wine's `C:` drive. Under Wine's `Z:`
(host filesystem) it correctly refused: Wine reports the `Z:\` drive root as a reparse point.

Wine needs a UTF-8 locale (`LANG=C.UTF-8`) for the Unicode-filename fixtures. Under the C locale
those fixtures fail because of Wine, not the product.

## 4. Directive coverage: what actually exists

Legend: **Tested** = executed and observed. **Impl.** = code exists, partially exercised.
**None** = absent.

| Directive area | Status | Evidence / gap |
| --- | --- | --- |
| Executable loading, relocation | None | No ELF/SELF loader exists |
| CPU translation / recompilation | None (see §5: not needed for PS4) | — |
| Memory, threading, console API (Orbis HLE) | None | No libkernel/libc/libScePad/… implementations |
| Filesystem / asset access | Impl. (read-only intake) | Folder inventory, PKG outer header, **new** dump validation; no content import |
| Graphics command translation, shaders | None | DX12 clears the backbuffer only; no GNM/PM4 or GCN shader path |
| Audio decoding / mixing | Impl. (test tone) | No AT9/ATRAC9 or streaming decoder |
| Input | Impl. | XInput 4-slot polling; no remapping, no DS4/DualSense HID, no game binding |
| Save data, timing, video/cutscenes | None / Impl. (fixed-step clock only) | — |
| Error handling, crash reports | **Tested (new)** | Crash JSON + minidump + next-launch warning |
| Game-version validation (launcher §10.2) | **Tested on synthetic data (new)** | `JStarsSetup.exe --validate-game`; manifest lists no builds |
| Gameplay, modes, roster, multiplayer | None | Cannot begin without an execution runtime |
| Launcher/installer | Tested (prior session) | Installs the shell only; GUI not rerun here |

## 5. Architecture: the shortest viable path

**Key technical fact.** J-Stars Victory VS+ on PS4 ships as x86-64 ELF code (wrapped in a SELF
container) for an AMD Jaguar CPU. A Windows x86-64 PC can execute that machine code natively.
What has to be rebuilt is everything around the code:

1. A loader for already-decrypted PS4 ELF images (SCE-specific segment and dynamic-symbol
   tables, NID-based import resolution, relocations, TLS).
2. High-level reimplementations of the Orbis OS libraries the game imports (kernel, threads,
   memory, filesystem, pad, user, save data, audio out, video out, system dialogs, …).
3. A GPU backend that interprets GNM/PM4 command buffers and recompiles GCN shaders to
   DXIL/SPIR-V. This is the largest single component.
4. Middleware: audio decode (AT9) and the game's video/cutscene format.

`docs/FEASIBILITY.md` says a PS4 executable "cannot simply be recompiled". That is true but
incomplete: no static recompilation is needed. The real cost is items 1–4.

This is how the open-source PS4 compatibility layer shadPS4 works (GPLv2, Vulkan backend,
translating PM4 command packets; per [the Emulation General wiki](https://emulation.gametechwiki.com/index.php/ShadPS4)).
The same source notes that games must be dumped and decrypted on a PS4, and that some titles
need system libraries dumped from the owner's console. This audit did not inspect the shadPS4
source and did not find a compatibility report for J-Stars Victory VS+, so whether it boots
there is **unknown**.

### Options (owner decision required)

| Option | What it means | Realistic outcome | Constraints |
| --- | --- | --- | --- |
| A. Original HLE runtime in this repo | Write 1–4 from scratch, game-specific | Years of work before the title screen | Clean licensing; highest effort |
| B. Game-specific runtime derived from shadPS4 | Fork, strip it to this title, add the launcher, settings, input and fixes from this repo | Fastest route to "it boots"; J-Stars-specific fixes become the real work | Whole product becomes **GPL-2.0**; close to what the directive forbids ("bundle shadPS4 and call it native") |
| C. Source port | Rebuild from the original source | Best fidelity | Requires original source and rights; not available |

The directive's own allowed list includes "a game-specific high-level compatibility runtime"
and "reimplementation of platform APIs". Options A and B both fit that description, but B
is the only one with a plausible near-term path. This is a licensing and product-identity
decision, so the audit records it rather than taking it.

### Legal boundary adopted in this checkpoint
No code decrypts PKG, PFS, SELF or firmware, and none contains keys. Intake accepts only
folders the owner has already dumped and decrypted from their own console, and the validator
reports a SELF container as "not inspected" rather than attempting it.

## 6. Prioritized roadmap

Gates are executed tests, not code existence.

| Milestone | Work | Gate (evidence required) | Blocked on |
| --- | --- | --- | --- |
| **M0 — this checkpoint** | Second-compiler build, test portability, crash reporter, dump validator | Section 3.3 | — |
| M1 — Close prior gaps | Run symlink tests (Developer Mode), D3D12 debug layer, MSVC + ASan build, rebuild shipped binaries from this source | Logs on the Windows 11 / RX 9070 XT host | Owner's Windows machine |
| M2 — Real intake | Validate the owner's decrypted dump with `--validate-game`; add the first manifest row as `untested` | Report JSON from the real dump | Owner's decrypted dump |
| M3 — Architecture | Choose option A or B (§5) and record an ADR | Written decision | **Owner** |
| M4 — Loader | Parse and map the decrypted eboot; list imported modules/NIDs; resolve against stubs that log every call | Real eboot reaches its entry point; import inventory | M2, M3 |
| M5 — Boot to first frame | Kernel/thread/memory/filesystem HLE; VideoOut + minimal GNM path | Title screen rendered, with screenshot | M4 |
| M6 — Menus and input | Pad HLE mapped from XInput/SDL3, remapping, two-player slot assignment | Menu navigation by controller on real hardware | M5 |
| M7 — Battle | Full GPU path for battle stages and effects; AT9 audio | One full match per character, compared with reference captures | M5, reference captures |
| M8 — Completion | Story/arcade completion, save round-trips, 60 FPS investigation, stability soak | Directive §14 criteria, each with evidence | M7 |

Online play, mods and higher-framerate enhancements come after M8. Shipping them earlier would
contradict the directive's "don't claim until tested" rule.

## 7. Known issues carried forward
- `JStarsReborn.exe` and `JStarsSetup.exe` in the package are the **0.2 binaries**. The 0.2.1
  source changes (crash reporter, `--validate-game`, `--crash-test`) are **not in them** until
  rebuilt with `docs/BUILD.md` on a Windows host (M1).
- Rendering waits on the GPU every frame with a single allocator and a `Sleep(1)` poll. This is
  correct but not tuned for frame pacing (documented as conservative). No profiling was done.
- XInput polls all four slots every frame, including empty ones. The cost is unmeasured on the target host.
- Runtime data defaults to the executable folder; installing under Program Files would break
  logging. Move to `%LOCALAPPDATA%` before any installer release.
- The PARAM.SFO parser is strict and has only seen synthetic files. A real file it rejects will
  report the exact reason; treat that as a parser bug to fix, not a bad dump.
