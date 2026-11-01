# gpu/ - the renderer library (GPL-2.0-or-later)

Static library `jstars_gpu` that gives the native runtime a PS4 graphics stack: shadPS4's Liverpool PM4
command processor, GCN to SPIR-V shader recompiler, buffer/texture caches and Vulkan renderer, plus its
GnmDriver, VideoOut and kernel event-queue libraries. Provenance, licenses and the integration boundary are
in `VENDOR.md`; every local change to a vendored file is in `patches/shadps4-local.patch`
(regenerate with `tools/make_patches.sh <upstream checkout at the pinned revision>`).

The runtime talks to it only through `jgpu.h` (plain C). Nothing in this directory contains game code,
keys, firmware or game content.

## Build (Linux development harness)

```
cmake -S gpu -B build-gpu -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-gpu                       # about 30 minutes on 2 cores
make -C runtime jstars-boot-gpu GPU_LIB=$PWD/build-gpu/libjstars_gpu.a
```

Needs: a C++23 compiler (GCC 13), `glslangValidator` (host shaders), Boost headers (ICL, Asio), X11 and the
Vulkan loader. Vulkan validation: `JSTARS_VK_VALIDATION=1` (core), `JSTARS_VK_VALIDATION_GPU=1`,
`JSTARS_VK_VALIDATION_SYNC=1`; the Khronos layer must be installed on the machine.

## Build (Windows x64 executable, cross-compiled with mingw-w64 on Linux; this is how the checkpoint binaries were made)

```
cmake -S gpu -B build-gpu-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/gpu/cmake/mingw-w64-x86_64.cmake
ninja -C build-gpu-win                   # four translation units are compiled with clang (see gpu/CMakeLists.txt)
make -C runtime jstars-boot-gpu.exe    GPU_LIB_WIN=$PWD/build-gpu-win/libjstars_gpu.a   # the game runtime
make -C runtime jstars-boot-gpu-hg.exe GPU_LIB_WIN=$PWD/build-gpu-win/libjstars_gpu.a   # diagnostic heap-guard build
```

The linker prints "duplicate section ... has different size" notes for C++ inline statics emitted by both compilers; the
Makefile passes `--allow-multiple-definition` for that reason (ADR-006).

## Run (headless Linux test with a software rasterizer)

```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
  runtime/jstars-boot-gpu --boot <prepared>/boot-linked.bin --log run.log \
  --gpu-dir out/gpu --capture-dir out/captures --capture-flips 30 --max-seconds 120
```

`runtime/jstars-boot-gpu --stop-at-first-gnm` is the unchanged 0.2.2 diagnostic mode: it never starts this
library and stops (exit 3) at the first graphics call.
