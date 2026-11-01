# Executed test report — native foundation 0.1

Test date: 2026-10-31, America/New_York. Raw runtime evidence records UTC timestamps. This is a Phase 0 assessment and Phase 1 foundation checkpoint, not a playable-game or release-candidate report.

## Hardware and build

Windows 11 Pro x64, build 26300; AMD Ryzen 7 9800X3D (16 logical processors); approximately 31.16 GiB OS-visible RAM. DXGI selected AMD Radeon RX 9070 XT; installed driver 32.0.32015.7029. Borderless resize reported a 3440x1440 desktop. Other display adapters were installed but not selected by the hardware tests. Evidence: hardware.json and gpu-driver.json.

Native C++20 Release build: LLVM-MinGW 20261020 / Clang 23.1.4, CMake 4.4.5, Ninja 1.13.2. Build succeeded without compiler warnings after fixes. PE inspection confirms AMD64, 64-bit native Windows GUI subsystem and OS API imports, with no console executable/VM/emulator dependency. See evidence/executable-inspection.txt. Windows 10 and MSVC builds were not executed.

## Executed checks

| Check | Result | Scope |
| --- | --- | --- |
| Core C++ regressions | PASS, 8,336 checks | 30/60/120/144 display cadence equivalence, irregular elapsed accounting, bounded stalls, invalid inputs, settings corruption/bounds/atomic replacement, concurrent JSONL |
| Input C++ regressions | PASS | Exhaustive stick/trigger normalization, bounds/monotonicity, unfocused neutral state, invalid parameters |
| Asset inventory Python tests | 13 PASS, 1 SKIP | Caps, hashing, paths, file changes, invalid inputs, reparse indicators; real symlink creation denied with Windows error 1314 |
| Native runtime harness | 10/10 PASS | Hardware 720p/repeat/1080p/1440p/4K, three resize transitions, borderless, explicit WARP, injected startup failure, rejected width 0 |
| Installed package launch | PASS | Supplied config loaded, 60 presents, clean shutdown; evidence/packaged-launch.jsonl |
| Audio device initialization | PASS in two harness sessions | XAudio2 mastering/source creation; audible playback not verified |
| Controller discovery | XInput slot 0 reported connected | Device identity, buttons, physical hot-plug and rumble not verified |
| Debug-layer availability | Diagnosed unavailable | --validation exited 1 with HRESULT 0x887a002d; no graphics-validation pass claimed |

The eight successful graphics cases each presented 60 frames and recorded clean shutdown. Two failure cases exited 1 as expected. Repeat launch loaded existing settings, preserved them, and appended a distinct log session. Borderless startup resized from 1280x720 to 3440x1440. Hardware cases did not fall back to WARP.

## Recorded timing samples

Each row contains 60 clear-only render/present section wall-time samples with default VSync. These include startup/transient scheduling and are not whole-frame gameplay timings or GPU timestamps. No performance target is certified. Window visibility, compositor and display refresh affect these short measurements. No baseline comparison was performed.

| Case | Initial buffer dimensions | Mean ms | p95 ms | p99 ms |
| --- | --- | ---: | ---: | ---: |
| hardware_720p_audio | 1280x720 | 11.933 | 16.505 | 30.561 |
| hardware_720p_repeat | 1280x720 | 8.833 | 15.707 | 29.980 |
| hardware_1080p | 1920x1080 | 9.742 | 15.768 | 31.897 |
| hardware_1440p | 2560x1440 | 8.241 | 15.842 | 16.168 |
| hardware_4k | 3840x2160 | 9.095 | 15.608 | 31.055 |
| hardware_resize | 1280x720 | 10.277 | 31.179 | 34.032 |
| hardware_borderless | 1280x720 | 12.218 | 15.986 | 16.069 |
| warp_720p | 1280x720 | 7.668 | 15.732 | 16.076 |

## Bugs found and repaired

The first resolution run passed eight cases and failed 1440p/4K: the OS limited client height to 1422. The window now supplies maximum tracking bounds via WM_GETMINMAXINFO; the final run verifies 2560x1440 and 3840x2160, rather than inferring resolution from arguments. Earlier failures are retained in work/smoke-final-001 and 002, outside the delivery package.

Review also found and fixed the startup borderless resize flag being discarded. The harness asserts the actual resize event. Input configuration VSync representation was corrected to the parser's documented 0/1 format. The asset inventory test exposed Windows deprecated ctime discrepancies across lstat/fstat; stable file identity, size and mtime are now used on Windows.

## Limits and remaining validation

No game data was provided; characters, animations, collision, combat, modes, progression, textures, mods and online services are not exercised or implemented. 4K clear buffers are not a 4K texture upgrade. No extended soak, match loop, memory-growth profile, CPU/GPU workload profile, packet-loss test, save-game integrity test, ASan, UBSan, static analyzer, manual input mapping, audible listening test or cross-machine matrix was performed. Graphics Tools is unavailable; debug-layer checks remain a dependency. Forced GPU removal/hang recovery is untested; automatic recovery is not implemented.

## Reproduce and inspect

Run the commands in BUILD.md. Read evidence/ctest.txt, evidence/runtime-smoke.json, evidence/validation-unavailable.jsonl and the PE inspection. The JSON report embeds each tested session's logs, command, dimensions, exit code and timings. The asset inventory tests use synthetic data only. Test failures are returned as nonzero exit codes.
