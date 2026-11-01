# Build, run and test

## Tested toolchain

Target: Windows 10 version 1703 or later / Windows 11 x64 (only Windows 11 was tested). C++20; LLVM-MinGW 20261020 / Clang 23.1.4 (UCRT), CMake 4.4.5, Ninja 1.13.2. Python 3.12.14 runs tool tests. Exact download URLs and SHA-256 digests are in toolchain-lock.json. These are build tools, not bundled game dependencies. Download official archives, verify their hashes, and extract to a tools directory. No system installation or PATH changes are necessary.

From the source root in PowerShell, replace the three C:/tools locations with the actual extracted locations:

~~~powershell
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/tools/llvm-mingw-20261020-ucrt-x86_64/bin/x86_64-w64-mingw32-clang++.exe -DCMAKE_MAKE_PROGRAM=C:/tools/ninja.exe
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
python tools/smoke_test.py --exe build/native/JStarsReborn.exe --output-dir build/smoke-001
cmake --install build/native --prefix build/package
python tools/make_setup_payload.py --package build/package
~~~

If cmake is not on PATH, invoke its absolute bin/cmake.exe path. Python discovery is optional for the C++ build; pass -DPython3_EXECUTABLE=C:/path/python.exe to include inventory tests in CTest. Smoke output directory must not already exist; each new run preserves evidence. The smoke harness opens short-lived native windows and requires an interactive Windows desktop with a DX12 driver. GUI executable invocations must be waited on (subprocess.run or Start-Process -Wait); a shell returning immediately does not prove success.

## Alternative MSVC build (not executed in this session)

Visual Studio 2022 with Desktop development with C++, a Windows 10/11 SDK, CMake 3.25+, optional Python 3.10+. In a developer shell:

~~~powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release
~~~

The supplied MSVC preset is a reproducible intended build route, not a verified MSVC result. -DREBORN_ASAN=ON configures MSVC AddressSanitizer; no sanitizer run is claimed here. UndefinedBehaviorSanitizer and static-analysis CI remain future validation work. D3D12 --validation requires Windows Graphics Tools; missing tools produce a diagnosed failure instead of silently disabling validation.

## Runtime switches

| Switch | Behavior |
| --- | --- |
| --data-dir PATH | Config, logs and crash-report root; default %LOCALAPPDATA%\JStarsReborn (Known Folder API). An explicit path disables legacy-settings migration |
| --width N / --height N | Initial client dimensions, 320..7680 / 240..4320 |
| --smoke-frames N | Exit cleanly after N presents, 1..1000000 |
| --warp | Explicit Microsoft software DX12 diagnostics; never selected silently |
| --validation | Enable DX12/DXGI debug layers before device creation |
| --no-audio | Skip XAudio2 initialization |
| --no-dialogs | Log failures without a blocking error dialog |
| --borderless | Start in monitor-sized borderless mode for this session |
| --resize-smoke | Three resize transitions; requires at least 40 smoke frames |
| --fail-init | Deliberately fail initialization for error-path regression |
| --crash-test access\|terminate | Deliberate crash to verify the crash reporter (writes crashes/*.json and *.dmp, exits with the exception code) |

Escape exits; F1 plays a procedural diagnostic tone if audio is available; F2 toggles and persists VSync; F3 toggles session-only borderless. Native DS4/DualSense HID support and remapping are not present. XInput-compatible devices or external mappings may appear as an XInput slot; that does not establish native PlayStation-controller support.

## Settings and diagnostics

config/settings.ini is a documented runtime-owned key=value format, unrelated to original game formats. Supported keys: width, height, vsync (0 or 1), fixed_hz (30..120), master_volume (0..1). Defaults: 1280,720,1,60,0.5. Unknown/duplicate/malformed/out-of-bounds values reject the whole file with a warning and use defaults. Existing rejected files are preserved unless a user changes a persisted setting; back them up before editing. Resize persists windowed dimensions. CLI dimensions affect this run and may be saved when new settings are first created. Display mode is session-only; no complete settings UI is advertised.

Logs include startup, selected adapter, actual initial client dimensions passed to the swapchain, audio/controller status, resize, failures, and clean shutdown. frame_summary measures render/present call wall time (first 60,000 samples), not whole gameplay frame time or a GPU timestamp. UTC log timestamps are explicit. A minimized window pauses the scheduler elapsed origin. Long stalls are bounded, dropped duration is logged, and no catch-up rate claim for original combat is made.

GPU waits have a five-second bound. Device errors are fatal diagnostics; automatic recovery, crash dumps, stack traces, shader compilation, and shader caches are not yet implemented. Exception-safe cleanup and normal fence synchronization are implemented, but forced GPU hangs have not been exercised.

## Cross build and portable tests (0.2.1)

A Linux cross build with GCC MinGW-w64 is verified (warning-free) via a CMake toolchain file that sets `CMAKE_SYSTEM_NAME Windows` and the `x86_64-w64-mingw32-*-posix` compilers. Ubuntu's MinGW lacks `libxaudio2_9.a`; generate one with `x86_64-w64-mingw32-dlltool` from a `.def` exporting `XAudio2Create` for link checks only. Run the CTest executables under Wine with `LANG=C.UTF-8`. Wine results are not Windows results.

`tools/portable_tests.sh [build-dir]` builds the platform-independent validator suite natively with ASan and UBSan and parses its JSON output with Python.

## Crash reports

The runtime installs an unhandled-exception filter and a std::terminate handler at startup. Reports go to `crashes/` under the data directory: a JSON file (exception code and name, faulting module+RVA, unwound stack frames as module+RVA, minidump path) and a `.dmp` minidump via the system dbghelp. The process then terminates with the original code. The next launch logs `previous_crash_reports`. Symbolize frames against the matching build. With `--no-dialogs`, no crash message box is shown.

## Setup menu

The 0.2 setup target uses the native Windows resource compiler as well as C++20. Portable LLVM-MinGW discovery locates its matching windres beside clang++; pass an explicit -DCMAKE_RC_COMPILER=C:/tools/llvm-mingw-20261020-ucrt-x86_64/bin/x86_64-w64-mingw32-windres.exe if needed. Use forward slashes in CMake tool paths. After install, run the manifest generator shown above before distributing the setup folder. See SETUP.md and SETUP_TEST_REPORT.md.
