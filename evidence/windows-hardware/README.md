# Windows hardware evidence (2026-10-31)

Machine: Windows 11 Pro 10.0.26300 x64, AMD Ryzen 7 9800X3D, AMD Radeon RX 9070 XT (driver 32.0.32015.7029); see `environment.txt`.
Binaries: cross-compiled with GCC 13 MinGW-w64 on Linux, copied to `E:\JStarsWork`, started from File Explorer
(`tools/windows_verify/run-all.cmd` -> `run-all.ps1`). The game folder `E:\CUSA01593` was only read.

## Two runs
* **Run 1** exposed two script bugs (empty argument list, exit codes not captured) and a loader bug: host symbols were
  matched through `import-names.tsv`, which was looked up with `/` separators only, so on Windows **0 host imports bound** and the
  first module initializer stopped at an unimplemented import (`boot-windows-run1.err.txt`). The loader now computes NIDs itself and
  does not need the name table (`runtime/jstars_rt.c`). Run 1 also produced `validation-run1.json`/`validate-game-run1.txt` (the exit
  code of that call was not recorded), the per-run runtime logs (`runtime-logs/`) and crash reports (`crash-reports/`).
* **Run 2** re-executed everything with the fixed loader and scripts: `summary-run2.json`, `test-*.txt`, `boot-windows.log`,
  `validate-game.err.txt`. The runtime logs and crash reports were not fetched again after run 2; `summary-run2.json` shows the same exit
  codes for those steps.

## Results
| Check | Result |
|---|---|
| core / input / paths / gamecheck / pkg_intake / setup_inventory / setup_install test suites | all exit 0 (8336 core checks, input regression, 13 path, 540 game-validation, 24 PKG-intake, 32 inventory, 121 installer checks) |
| symlink/reparse fixtures (paths, gamecheck, inventory, installer) | **SKIPPED**: Windows error 1314 (no privilege to create links). Not a pass. |
| `JStarsSetup.exe --validate-game E:\CUSA01593` | run 1: report written, `TITLE_ID=CUSA01593 APP_VER=01.02 executable=self build=untested`. Run 2: exit 1 only because the report file already existed (exclusive create, by design). |
| `JStarsReborn.exe` smoke (clear-only DX12 foundation, 120 frames, no game data) | ran on the RX 9070 XT, mean 6.9 ms; XAudio2 initialised; XInput slot 0 connected; clean shutdown. This is the old foundation, not the game. |
| resize smoke | clean shutdown (exit 0) |
| D3D12 debug layer (`--validation`) | **Not exercised**: exit 1, HRESULT 0x887a002d (DXGI_ERROR_SDK_COMPONENT_MISSING): the Windows "Graphics Tools" optional feature is not installed. |
| crash reporter | `--crash-test access` -> exit 0xC0000005, JSON + minidump; `terminate` -> exit 0xE0524E54, JSON + minidump |
| game execution (`bin\jstars-boot.exe`, run 2) | 112 host + 94 game-module imports bound, 271 trapped; both module initializers return 0; entry point executes; 15 `sceSysmoduleLoadModule` calls and `sceVideoOutOpen` served; stops at the first Gnm call (`sceGnmDrawInitDefaultHardwareState200`, exit 3) as designed |

Not covered: setup GUI interaction by a person, audible output, controller input response, any frame of the game, MSVC/ASan builds.
Timestamps in `boot-windows.log` are **milliseconds** since start (the whole boot is about 30 ms).
