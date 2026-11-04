# Change log

## 0.5.4 playtest preview (pre-release)

- **Smoother frame pacing.** At 60 fps, frames alternated between 16 and 31 ms (Windows' 15.6 ms timer tick): the renderer's frame loop
  slept on a low-resolution timer. The runtime now asks Windows for 1 ms timer resolution and the frame loop uses a high-resolution
  timer. Measured on an RX 9070 XT at 60 fps (5-minute scripted run, menus and a Free Battle; medians of 5-second
  windows): frame time p95 30.6 -> 17.3 ms; p99 31.5 -> 17.4 ms in menus and 31.6 -> 24.1 ms in battle. This also
  helps at 30 fps. The default present mode is Mailbox where the driver offers it (FIFO otherwise).
- **Ultrawide / widescreen (experimental, Settings):** on screens wider than 16:9 the game shows more to the sides (Hor+) instead of
  black bars, with the vertical view unchanged; at 3440x1440 the horizontal field of view goes from 60 to about 76 degrees. The 2D HUD
  and menus are stretched for now. Applied in memory only after checking the game build. `docs/WIDESCREEN.md`.
- **Fullscreen (Settings):** borderless fullscreen on the monitor. Alt+Enter or F11 switches between fullscreen and the window.
- **Smooth pacing on high-refresh monitors (Settings, on by default, fullscreen only):** at 144/165/175 Hz without variable refresh,
  30/60 fps frames stay on screen for uneven times, which looks like stutter even at a steady 60 fps. In fullscreen the monitor now
  runs at 120 Hz (a multiple of 60) while the game plays, and goes back to its normal rate when you leave fullscreen or close the
  game. The game log names the display mode.
- **AMD FSR 1 upscaling (Settings, on by default):** the game's 1280x720 frame is upscaled with FSR 1 (EASU) and sharpened (RCAS)
  instead of bilinear scaling. FSR 4 and frame generation need motion vectors, which the game does not render (ongoing work).
- **Shader cache:** shaders and pipelines compiled while playing are saved (`%LOCALAPPDATA%\JStarsReborn\shader-cache`) and rebuilt
  when the game starts next time, instead of being compiled the first time an effect appears in battle (each was a 30-160 ms hitch).
  The first session with a new version still compiles as it goes. The cache of an older version is removed automatically.
- A minimized game no longer slows down (an extra 8 ms wait per dropped frame made it run at ~44 fps).
- **PlayStation controllers without DS4Windows or Steam Input:** DualSense, DualSense Edge and DualShock 4 over USB or Bluetooth are
  read directly, including analog triggers, the touchpad click and rumble (contributed by CLAIREANDLEON, PR #5; tested with a DualSense
  over USB; Bluetooth and DualShock 4 not yet confirmed on hardware). XInput pads and the keyboard work as before.
- **Diagnostic reports** (Troubleshooting > Create diagnostic report) keep the start of each game log (GPU and driver details), record
  the 60 fps / fullscreen / ultrawide settings of each run, and also remove game-folder paths, a custom player name and friends' room
  addresses. Play sessions now log frame timing every 5 seconds.
- Launcher 0.5.4: "Fullscreen" and "Ultrawide / widescreen" settings.

## 0.5.3 playtest preview (pre-release)

- **Launcher: new Saves page.** Import a save (a SAVE-DATA.DAT, or a folder that contains one: another JStarsReborn install, a shadPS4
  user folder, a decrypted PS4 save), **Unlock every character** (all characters unlocked and owned, J-Points 999,999), and restore any
  earlier save. Every change backs up the current save first (`%LOCALAPPDATA%\JStarsReborn\save-backups`). The save is checked (size and
  checksum) before it is used; encrypted PS4 saves are recognised and explained. Read-only saves (copied from a USB stick or a ZIP) work.
- The save format (checksum, character and J-Point offsets) is from CLAIREANDLEON's save editor (`tools/save_editor`, PR #3), checked
  against saves the game wrote in our test runs.
- Runtime unchanged from 0.5.2 apart from its version number.


## 0.5.2 playtest preview (pre-release)

From the 0.5.1 playtest reports (recomplauncher.com):
- **Online:** the second player's game no longer stops when Search for a Room finds the host's room (`sceNpMatching2SignalingGetPingInfo`
  was missing). Joining a room is still not implemented, so a match between two players cannot start yet.
- **Audio:** the sound feed now runs with Windows' real-time audio scheduling (MMCSS "Pro Audio") and refills 20 ms after a dropout instead
  of resuming with tiny gaps. This targets the "choppy/scratchy" reports. It could not be reproduced on our test PC (0 underruns before
  and after), so it is unconfirmed.
- **Errors the game reports:** the system error dialog is implemented (logged; no on-screen popup), so a sign-in or network error returns
  to the menu instead of stopping the game.
- **60 fps (experimental, Settings):** the game's own engine rate set to 60 plus its six fixed 30 Hz timing constants, applied in memory
  only after checking the game build. Battle speed is correct at 60. Some animations or effects may still run at the wrong speed; please
  report them. 120 fps is not supported (the game's timers cannot run exactly at 120); the planned route is 60 fps plus frame generation.
- Launcher 0.5.2: new "60 fps (experimental)" setting.


## 0.5.1 playtest preview (pre-release; online still in testing)

- Online is peer-to-peer over the internet, not only the local network: a host's room code (its public address) lets another player
  see its rooms; the launcher opens the two UDP ports with UPnP (or they are forwarded by hand). No server is involved.
- Launcher: new **Online** page (on/off, UPnP, "Get my room code", "Friend's room code"). Opened ports are closed when the launcher exits.
- Runtime: room-directory queries (`--lobby-peer ip[:port]`), answered to the asker's address so they pass the asker's NAT.
- Still missing: joining a room (so no match between two players yet), Rank Match, Quick Match.


## 0.5.0 playtest preview (branch `feature/online-np`; pre-release)

A match between two PCs is **not** possible yet (joining a room is missing); nothing here contacts PlayStation Network. Test results so far are
from one PC.

- `--online`: real UDP sockets for `libSceNet` (console address layout, FreeBSD errno), virtual-port (P2P) multiplexing because the game
  uses socket type 6, `libSceNetCtl` reporting this host's address. Off by default.
- A local signed-in identity, NP state callback, request API, account country/language, WebApi (only the player's own block list is
  answered, locally) and a session-local Score board, so the game's Online Battle path runs instead of stopping.
- Matching2 and Signaling context lifetime, plus Matching2 request IDs and asynchronous response callbacks.
  `sceNpMatching2GetWorldInfoList` returns one local world with the number of rooms heard on the network; the online-j Windows probe reaches the game's
  Online Battle menus. Host tests: 278/0 on Linux and Windows at `711045b`. The probe did not exercise a room request;
  room operations, peer connections and multiplayer remain unverified.
- Native Windows host-runtime build tool (`tools/windows_build/`), verified with llvm-mingw Clang 23.1.4 and 266/0 checks at
  `656e15c`. Native GPU linking still requires a compatible renderer/toolchain; game runs use the existing Linux cross-build.
- `--trace-net`, `--trace-np` (bounded tracing) and `--experiment online-probe` (diagnosis only).
- Fix: closing a P2P socket released neither its queue nor the UDP port.
- Every net/NP import has an audit row; the 0.4.0 socket rows were stale.
- LAN room directory and Friendly Match hosting: Create a Room works and is announced on the local network; Search for a Room lists
  rooms other PCs announce. Joining a room, Rank Match and Quick Match are not implemented yet.
- Launcher: Settings > "Online test features (experimental)" and an optional "Other PC's address". Off by default.

## 0.4.0 — 2026-11-01

**New launcher `JStarsReborn.exe`** around the verified runtime of checkpoint 0.3.0 (build w), which is kept unchanged as the
baseline (`/mnt/user-data/outputs/jstars-0.3.0-package/`, `evidence/windows-gpu-0.3.0/`). The 0.2.2 `JStarsReborn.exe`
(DX12 foundation demo) is not part of this release; its source target is now `JStarsFoundation`.

- Launcher (Win32, per-monitor DPI aware, keyboard navigable): Play (enabled only when the game folder validates and the
  runtime and Vulkan loader are present), Game folder (folder picker, remembered, read-only validation against the
  supported build: CUSA01593 01.02 by eboot/libc/Fios2 SHA-256), Settings (only what the runtime applies: window size,
  sound on/off, volume, keyboard as controller, confirm button, player name), Controls (the real mapping), Troubleshooting
  (latest session logs, log folder, diagnostic report ZIP, delete prepared data, data folder, licenses).
- Normal Play session: the runtime is started directly (no command window, explicit working directory = the session folder,
  quoted paths, UTF-8-safe arguments) with live input only: no `--pad-script`, no `--max-seconds`, no captures, no
  benchmark/diagnostic switches; developer `JSTARS_*` environment variables are removed. One session at a time (single
  launcher instance; a game window found on screen blocks a second start). The process is tracked until it exits; Stop game
  sends a normal close to the game window and asks before ending an unresponsive game. Exit codes are explained in context
  (0 = closed by the player; 3 = unsupported console function, named; 4 = crash; 5/6 = graphics; 7 = test time limit, never
  used by the launcher).
- First-run preparation is now native (`src/prepare`, port of `tools/prepare`): byte-identical `boot-linked.bin` and
  `import-names.tsv` to the Python tools for the same dump (`tools/prepare_native_check.sh`). Prepared data is cached under
  `%LOCALAPPDATA%\JStarsReborn\prepared\` keyed by the input hashes, verified before each use, rebuilt when inputs change,
  written atomically and cancellable.
- Per-user data in `%LOCALAPPDATA%\JStarsReborn`: `launcher.ini`, saves in `user\`, one folder per session in `sessions\`
  (game.log, stdout/stderr, `session.json` with versions, settings, command line, start/end time, exit code and explanation;
  newest 30 kept), `logs\launcher.log`. Data from an earlier version is copied to a `backup-before-launcher-*` folder on
  first start; the 0.2.2 window size is imported, its volume (a test tone) is not.
- Runtime 0.4.0 (`runtime\jstars-runtime.exe`): `--version`, `--volume 0-100`, UTF-8 command line and file opens on Windows;
  live input only while the game window is in the foreground; the active XInput controller is read every frame and empty
  slots are probed every 500 ms (reconnects are picked up, a removed controller is dropped at once); keyboard Esc = OPTIONS,
  I/J/K/L = right stick (in addition to the numpad).
- Renderer: the `Bc5UnormBlock ... Storage is not supported` errors are fixed at their cause (verified with `tools/vkprobe`
  on the RX 9070 XT: the driver rejects storage usage through block-texel views for BC5 and BC6H, accepts it for BC1-4/BC7;
  such images are now created without storage usage). The Vulkan validation message VUID-VkBufferCreateInfo-size-06409 is
  traced: the buffer cache creates 4 GiB sparse arena buffers while this device reports maxBufferSize 2 GiB; it is the
  unchanged upstream shadPS4 design and is **not** fixed in 0.4.0 (see Known issues).

- Launcher polish after testing the packaged copy on Windows: every button is drawn with a clear border and a visible disabled
  state, Enter presses the focused button, the diagnostic-report dialog starts in Documents (it remembers its own folder,
  separately from the game-folder picker) and refuses to save inside the game folder, and choosing the folder one level above
  a dump now names the subfolder that holds the game.

Tests of the packaged copy on the owner's Windows PC, unzipped to `E:\JStarsWork\JStarsReborn 0.4.0\`
(`evidence/windows-launcher-0.4.0/`; launcher release candidates 1 and 2, then the final launcher): first run with
migration backup, automatic default folder, invalid-folder messages, preparation, Play, normal exit, relaunch,
duplicate-launch guard and diagnostic report contents (candidates); settings kept across launcher restarts, Enter on Play,
the report dialog refusing the game folder, a deliberately damaged prepared image (readable "could not start" report, then
Delete prepared data and a clean restart) and a final Play/close (final launcher). The release runtime (sha256 `25bc88b9...`) passed the runner: start-up check exit 3 at
`sceGnmDrawInitDefaultHardwareState200` as before, host self-test 134/0, a 300 s scripted Free Battle with no fault and no
renderer error, and the same run under the validation layer with only VUID-06409 reported.

Known issues (0.4.0):
- **Human play and listening are not confirmed by this project.** In launcher sessions on the owner's PC an XInput controller
  drove the game's own flows (save prompt, menus, a Free Battle to its result screen; about 700 logged transitions), and
  keyboard presses reach the runtime, but keyboard play has not been seen to work end to end (see docs/INPUT.md). Whether it
  plays well, and whether the audio sounds right, is for the owner to confirm. The audio soft limiter only keeps the downmix
  in range; it says nothing about how the game sounds.
- The title screen ignores presses in some phases of its idle cycle (seen with the controller as well); press again a moment
  later.
- Vulkan validation `VUID-VkBufferCreateInfo-size-06409` (two 4 GiB buffer creations per run) is **not fixed**: the vendored buffer cache reserves
  the console address space in 4 GiB sparse arena buffers (`ARENA_PAGE_BITS = 32`, merged arenas can be larger), while the
  RX 9070 XT reports `maxBufferSize` = 2 GiB. The driver creates them anyway and no rendering fault has been traced to it, but it
  is outside the specification; a fix means changing the arena design, which was left for a separate renderer change.
- The System V red-zone risk on Windows page-tracking faults (ADR-006) remains; no symptom has been observed.
- The earlier unexplained crash `ace318` is not explained (it may have been the fixed mutex bug; not established).
- Movies are skipped (no decoder); trophies are an empty set; online/NP features are not available; only CUSA01593 01.02 runs.
- Only the paths exercised so far are covered: title, save prompt, menus, Free Battle and the J-Adventure world map (scripted),
  plus whatever the owner plays. Other modes are untested.
- `stderr.txt` in a session folder repeats `game.log` (the runtime logs to both); the diagnostic report stores it once.
- Ending an unresponsive game (the launcher's confirmation, then TerminateProcess) is implemented but could not be exercised:
  no hang occurred during testing.

## 0.3.0 checkpoint (build w; unpublished, not a release) — 2026-11-01

Execution runtime + shadPS4-derived Vulkan renderer (ADR-006, `gpu/`, pinned shadPS4 b4e7ae77). The verified 0.2.2 checkpoint is preserved outside the repo (`/home/claude/baselines/JStarsReborn-0.2.2/`) and `--stop-at-first-gnm` exits 3 at `0H2vBYbTLHI#libSceGnmDriver` (`eboot.bin+0x45e3a9`) with build w, as in 0.2.2.

**Highest game-execution milestone, observed on Windows 11 hardware (Ryzen 7 9800X3D, RX 9070 XT) from the user's own decrypted dump `E:\CUSA01593` (read-only):** the original game code, with native replacements for the console OS, GPU, audio and input, ran a whole Free Battle: copyright screen, title, the game's own save-data prompt, main menu, character select, stage select, loading, battle intro, a live fight with HUD, K.O., Victory Burst, victory pose, "Fight the battle again?" and the post-match menu, then sat on that menu until the 620 s time limit (18,609 flips, 8.88 M draws, 1.26 M dispatches, exit 7 = `--max-seconds`). Every frame was rendered by the vendored renderer from the game's own command buffers and presented in a window. **All input was scripted test input (`--pad-script`), not a controller or keyboard; a human has not played it.** Evidence: `evidence/windows-gpu-0.3.0/`.

What changed since the 0.2.2 checkpoint:
- Renderer: shadPS4-derived command processor, shader translator and Vulkan renderer linked into the Windows exe (see ADR-006 / `gpu/VENDOR.md` for dependencies, licences and boundaries; local modifications in `gpu/patches/shadps4-local.patch`, 9 files). New this segment: the presenter drops frames while the presentation surface has no size (minimised/hidden window) instead of aborting (one run, 093cfa, had died with a zero-size frame image; the trigger was never proven).
- Input: scripted pad (`--pad-script`, logged as `SCRIPTED TEST INPUT`), XInput and keyboard sources. Only the scripted source has been exercised.
- Audio: the game's CRI ADX2 mixer runs on its own threads; `sceAudioOut*` downmixes the 8-channel float ports to stereo (soft limiter) and plays through WASAPI shared mode; `--dump-audio` writes WAVs. The game's own mix goes above full scale in battle (one input channel up to 2.67, stereo downmix up to ~3.2); the limiter engaged in 66 of 126 five-second windows and ~0.25 % of dumped samples are at full scale. **Nobody has listened to it yet.** Over the 620 s run: 0 device underruns.
- `sceAvPlayer*`: diagnostic stub with the game's observed protocol (**movies are not decoded or shown**). Trophies: an explicitly EMPTY registered set (the trophy pack is encrypted and not read); `sceNpTrophyUnlockTrophy` (first reached in a battle; it stopped run bd7ec2 with exit 3) now answers INVALID_TROPHY_ID with a loud log. Save data mount/create/write works through the game's own flow.
- **Intermittent start-up crash found and fixed.** About 2 % of Windows starts crashed in `free()` (13 of 660 loop starts on builds t and v, exit 4, identical signature). A debug-only guard-page allocator (`runtime/jstars_heapguard.c`, `make jstars-boot-gpu-hg.exe`) located it deterministically: the game's libc writes at `[obj+0x20]` of the object behind every pthread mutex right after `scePthreadMutexInit`, and the host object was 8 bytes on Windows. Mutex/cond/rwlock/attr slots now point at padded blocks. Build w: 0/200 failures (same loop: build v 3/200); the heap-guard build ran the same path 200 times and a 60 s game run (1,799 flips) without a trap. The earlier unexplained crash `ace318` (run folder kept) may have been the same bug; this is **not** established.
- Diagnostics: faulting thread name and host return addresses with module+offset; first-call tracing of every import (NID, name, library, who implements it, audit status, caller, first argument); per-thread import counts; WASAPI/AudioOut level meter; `--no-audio`, `--dump-audio`, `--sample-threads`, `--count-imports`, `--trace-draws`; Vulkan validation layer through the runner (`JSTARS_VK_VALIDATION=1`).
- Runners (`tools/windows_verify/`): `run-gpu.ps1` (unique result folder per run; stdout/stderr/exit code/`COMPLETE.json` for every step even on failure; optional heap-guard steps) and `regress-loop.ps1` (repeated start-up check, keeps the log of every run that does not exit 3). Every run folder is kept.
- Import audit regenerated for this checkpoint (`docs/IMPORT_AUDIT.md/.tsv`): **477 unique import-table entries** — 94 original game-module code, 58 implemented with verified behaviour, 145 partially implemented, 88 diagnostic stubs, 92 unsupported (trap, exit 3, named). The 26,919 relocation records are uses of those 477 entries, not a count of functions. 131 of the 477 were called in the recorded runs.
- Host self-test: 134 checks, 0 failed (Linux, and each Windows runner). Build: four gpu translation units are compiled with clang (mingw GCC 13 internal compiler errors); `jstars_input.c` passes structs by pointer for the same reason.

Known gaps and open items (not claims of completeness):
- A human has not played it: controller/keyboard control is untested; audio has not been listened to; the level/limiter choice is open.
- Movies are skipped. Trophies are an empty set. Online/NP features answer offline or (92 imports) trap if reached; the dialogs, DiscMap, NP requests/commerce are not implemented.
- Renderer: ~70 per battle run `image.cpp:166 Bc5UnormBlock ... Storage not supported` errors (image creation continues; whether any texture is wrong is not checked); the Vulkan validation layer's 4 GiB-buffer VUID is not investigated.
- Windows page-tracking faults in guest leaf functions can in principle clobber the System V red zone (ADR-006); no symptom has been observed in the recorded runs, but the protection does not exist.
- Only the paths exercised so far are covered: title, menus, Free Battle, J-Adventure world map. Other modes, the result/ranking screens beyond the post-match menu, replay, online and mods are untested.
- A persistent user folder across launches (second-launch save load) was not retested with build w.
- No claim of complete decompilation, native performance or full playability is made.

## 0.2.2 — 2026-10-31

- Writable data moved to `%LOCALAPPDATA%\JStarsReborn` (Known Folder API): settings, logs, crash reports, and setup UI reports. A one-time, copy-only migration copies `config\settings.ini` from beside the executable when no per-user settings exist. It never overwrites or deletes either file, and it does not follow links. `--data-dir` still overrides and skips migration.
- The validator now understands SELF containers. It locates the embedded ELF header after the 32-byte SELF header and 32-byte segment entries, reports segment count and how many segments carry the encrypted flag, and states that payloads are never examined. A SELF whose embedded ELF header is unreadable is now a problem instead of a warning. Previously the validator implied that only a plain ELF could be used, which contradicts how upstream PS4 loaders read decrypted SELF dumps.
- Rebuilt `JStarsReborn.exe` and `JStarsSetup.exe` from source with GCC 13 MinGW-w64 (statically linked; only system DLLs). Regenerated `setup-payload.txt`, then `checkpoint-manifest.json`.
- New tests: `paths_tests` (migration), SELF fixtures, and 50,000-input fuzzing of the executable-header inspector.
- Added docs/ADR-005-execution-runtime.md (accepted on the owner's direction: follow the inFAMOUS Second Son PC-port architecture; nothing published).
- **Execution runtime (Linux development harness, `runtime/`, `tools/prepare/`, `tools/boot_smoke.sh`)**. Prepares and links the user's CUSA01593 v01.02 eboot with its bundled libc.prx and libSceFios2.prx into a work store keyed by title, version and eboot hash (the game folder is only read), binds imports (112 host + 94 game-module; 271 trapped), and runs the original code. Observed: both module initializers return 0, the eboot entry point executes, and the game proceeds through system/user/pad/VideoOut/AudioOut setup until the first Gnm call (`sceGnmDrawInitDefaultHardwareState200`), where the loader stops by design. No frame, audio, input or gameplay yet. Not yet run on Windows: the Windows host layer is not written. See `docs/IMPORT_STATUS.md`, `runtime/README.md`, `evidence/execution/`.
- Runtime OS abstraction (`runtime/jstars_os.*`) and a Win32 build (`make jstars-boot.exe`): VirtualAlloc/VirtualProtect, file-mapping backed direct memory, vectored exception handler, `sysv_abi` guest ABI. Under Wine 9 the Windows build reaches the same stop (module initializers return 0, entry point runs, first Gnm call traps). Evidence: `evidence/execution/boot-CUSA01593-01.02-win32-build-under-wine.log`. Host symbol lookup is now by NID computed in the runtime (no dependency on the optional name table). **Also run on Windows 11 hardware** (Ryzen 7 9800X3D, RX 9070 XT): same result natively, plus the 7 native suites (symlink fixtures skipped, error 1314), DX12 clear smoke, XAudio2/XInput init, resize and crash-reporter runs. D3D12 debug-layer run not possible (Graphics Tools feature missing). Evidence in `evidence/windows-hardware/`.
- `supported-versions.tsv`: first entry from observed data (CUSA01593 01.02, `untested`).

## 0.2.1 audit checkpoint — 2026-10-31

Independent audit (PROJECT_AUDIT.md). Rebuilt with a second compiler (GCC 13 MinGW-w64) and ran all native suites under Wine 9.0. Fixed: case-sensitive `Xinput.h` include that blocked cross builds; four misleading-indentation warnings; libstdc++ trailing-separator assumption in two test fixtures; a symlink test that treated an unmaterialized link as real. Confirmed that the symlink-rejection paths have never run against a real link on any host.

Added a portable SHA-256, a strict bounds-checked PARAM.SFO parser, an (empty) supported-build manifest and `JStarsSetup.exe --validate-game` for read-only checks of an already-decrypted dump. Tests use synthetic fixtures with 50,000-input mutation fuzzing under ASan/UBSan (`tools/portable_tests.sh`). Added a crash reporter (JSON with an unwound module+RVA stack plus a minidump; terminates with the original code; logged on next launch) and `--crash-test access|terminate`. No decryption, no game-content execution and no playability claims. The packaged .exe files are still the 0.2 binaries; rebuild to include 0.2.1 code.

## 0.2 native setup — 2026-10-31

Added the native Windows setup menu, PKG source selection/metadata registration, cancellable folder inventory and transaction-based foundation installation with SHA-256 checks. Added new tests and native UI execution evidence. This version does not unpack/decrypt PKG payloads or produce a playable game. Preserved the existing diagnostic runtime and 0.1 archive.

## 0.1 foundation — 2026-10-31

Created a native Windows C++20/CMake project; DX12 device/swapchain/resize/present; XInput normalization and four-slot polling; XAudio2 initialization and procedural tone; bounded fixed-step clock; validated atomic settings; synchronized JSONL diagnostics. Added regression tests and a read-only bounded asset inventory tool. Recorded feasibility, input blockers and an observable-reference protocol.

Fixed during review: missing explicit COM header; startup borderless resize event discarded before renderer update; unchanged resize spuriously marking settings dirty; default VSync config representation; desktop-capped 1440p/4K client dimensions; Windows inventory false file-change rejection from deprecated ctime values.

This checkpoint implements the native foundation only. No original-game functionality, import conversion, mod execution, or dedicated server has been marked complete. Test details and residual limitations are in docs/TEST_REPORT.md. Exact checkpoint files are in checkpoint-manifest.json.
