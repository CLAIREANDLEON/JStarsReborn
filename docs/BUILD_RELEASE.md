# Building the JStarsReborn 0.4.0 release (Windows x64)

The release is cross-compiled on Linux (Ubuntu 24.04 was used) with mingw-w64. Nothing in the build reads or embeds
game files.

## Tools

- `g++-mingw-w64-x86-64-posix` / `gcc-mingw-w64-x86-64-posix` 13 (posix threads), `binutils-mingw-w64-x86-64`
- `clang-18` (four GPU translation units hit internal compiler errors in mingw GCC 13; `gpu/CMakeLists.txt` builds them with clang)
- CMake >= 3.25, Ninja, `glslang-tools` (`glslangValidator`, host shaders), Boost headers 1.83 (ICL, Asio), Python 3 (packaging)

## 1. GPU library (shadPS4-derived renderer, `gpu/`)

```
cmake -S gpu -B build-gpu-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/gpu/cmake/mingw-w64-x86_64.cmake
ninja -C build-gpu-win            # about 30 minutes on 2 cores
```

`gpu/vendor/shadps4` is shadPS4 at `b4e7ae776071ceb1dd35004577a8719481e1d258` with the local changes in
`gpu/patches/shadps4-local.patch` already applied (regenerate the patch with `tools/make_patches.sh`).

## 2. Release folder and ZIP

```
tools/build_release.sh build-gpu-win/libjstars_gpu.a build/release
```

This builds the launcher (`tools/build_launcher.sh`, sources in `src/launcher`, `src/prepare`, `src/setup`, `src/core/paths.cpp`),
links the runtime (`make -C runtime jstars-boot-gpu.exe`, renamed to `runtime\jstars-runtime.exe`), and assembles
`JStarsReborn-0.4.0/` with the license texts, this source archive, `VERSION.txt` and `SHA256SUMS.txt`, then zips it.
Both executables are statically linked and import only Windows system DLLs; the Vulkan loader (`vulkan-1.dll`) comes
with the graphics driver and is loaded at run time.

A Windows-native CMake build of the launcher alone also works (`cmake -S . -B build` with MinGW or the mingw toolchain
file, target `JStarsReborn`).

## Checks that can run on Linux

```
tools/prepare_native_check.sh <game-folder> <shadPS4 aerolib.inl>   # native preparation == Python reference tools, byte for byte
g++ -std=c++20 -Iinclude -Isrc/prepare -Isrc/launcher tests/launcher_core_tests.cpp src/launcher/launcher_core.cpp \
    src/prepare/prepare.cpp src/setup/sha256.cpp src/setup/game_validate.cpp -o launcher_core_tests && ./launcher_core_tests /tmp/lct <game-folder>
# save page logic (synthetic saves; optionally a real SAVE-DATA.DAT as argument)
g++ -std=c++20 -Isrc/launcher tests/saves_tests.cpp src/launcher/saves.cpp -o saves_tests && ./saves_tests
make -C runtime jstars-boot && runtime/jstars-boot --selftest-host --log /tmp/selftest.log
```

The Windows verification runners are in `tools/windows_verify/` (`run-gpu.ps1`, `regress-loop.ps1`).
