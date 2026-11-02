# Third-party notices

## JStarsReborn 0.4.0 release (launcher + runtime)

`JStarsReborn.exe` (launcher) and `runtime\jstars-runtime.exe` were cross-compiled with mingw-w64 GCC 13 (posix
threads; libstdc++, libgcc and winpthreads statically linked under the GCC Runtime Library Exception and the
mingw-w64 runtime terms, texts in `LICENSES/third-party/`); four renderer translation units were compiled with
Clang 18. Both import only Windows system DLLs. The runtime links the shadPS4-derived renderer described below and is
therefore GPL-2.0-or-later as a whole (`LICENSES/GPL-2.0.txt`); the complete corresponding source is in `source/`.
The launcher's preparation step is a C++ port of `tools/prepare` (adapted from HayatoG/iss_pc, GPL-2.0-or-later)
and contains a list of PS4 function names (NIDs) for this game's imports taken from shadPS4's public NID table
(GPL-2.0-or-later), used only to label log lines. The launcher icon is original artwork made for this project; the
launcher may show the game icon from the player's own game folder, read from disk at run time and never copied.

## Earlier checkpoints (0.2.x foundation)

The included Windows executable was compiled with LLVM-MinGW 20261020 (Clang 23.1.4, UCRT), with statically linked toolchain C++ support. LLVM's full license and exceptions are included in licenses/LLVM-LICENSE.txt. MinGW-w64 attribution and runtime terms are included in the other licenses/ files, obtained from the project's official repository on 2026-10-31. Upstream files have not been modified.

Sources: [LLVM-MinGW release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261020), [MinGW-w64 general terms](https://github.com/mingw-w64/mingw-w64/blob/master/COPYING), [runtime terms](https://github.com/mingw-w64/mingw-w64/blob/master/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt), [additional notices](https://github.com/mingw-w64/mingw-w64/blob/master/COPYING.MinGW-w64/COPYING.MinGW-w64.txt).

Windows supplies DirectX 12, DXGI, XAudio2, XInput, Win32 and Universal CRT DLLs. These DLLs are not copied into this package. CMake, Ninja and Python are build/test tools and are not redistributed. No third-party game engine, emulator, proprietary console SDK, game executable, game asset or original soundtrack is included.

The project name identifies the requested compatibility target. This foundation does not represent an official publisher release or convey rights to the original game's content.

PKG field documentation references LibOrbisPkg for limited outer-header facts. No LibOrbisPkg source, binaries, keys, or decryption implementation are bundled.

## Execution runtime provenance (GPL-2.0-or-later)

`tools/prepare/{prepare,link_libc,link_modules}.py` are adapted from HayatoG/iss_pc (`port/scripts`), a GPL-2.0-or-later project, with title-specific parts removed. `tools/prepare/nid_names.py` reads shadPS4's public NID table (GPL-2.0-or-later) at the user's request to label log lines; the table is not copied into this repository. `runtime/` follows the structure of iss_pc's `probe.c` but is written for this title; struct layouts for system services were cross-checked against shadPS4 headers. The Castlesea/infamous-second-son-pc-port project was studied for architecture only; no code from it is included. Because adapted GPL code is present, the runtime directories are GPL-2.0-or-later. Nothing has been distributed.

## Renderer stack (`gpu/`, GPL-2.0-or-later)

`gpu/vendor/shadps4/` contains unmodified copies of files from shadPS4 (https://github.com/shadps4-emu/shadPS4, GPL-2.0-or-later) at commit `b4e7ae776071ceb1dd35004577a8719481e1d258`: the Liverpool PM4 command processor and Vulkan renderer (`video_core`), the GCN-to-SPIR-V shader recompiler (`shader_recompiler`), the GnmDriver/VideoOut/event-queue libraries and a subset of `common`. Every local change is a unified diff in `gpu/patches/shadps4-local.patch`. The integration layer in `gpu/shim/` and `gpu/jgpu*.cpp` replaces shadPS4's emulator core; parts of it follow the shims of HayatoG/iss_pc (`port/gpu/shim`, GPL-2.0-or-later) and the rest is written for this project. The Castlesea/infamous-second-son-pc-port project was studied for architecture only.

Third-party libraries under `gpu/vendor/externals/` (revisions and licenses in `gpu/VENDOR.md`): Vulkan-Headers (Apache-2.0/MIT), VulkanMemoryAllocator (MIT), sirit + SPIRV-Headers (BSD-2-Clause/MIT), fmt (MIT), robin-map (MIT), xbyak (BSD-3-Clause), magic_enum (MIT), half (MIT), xxHash (BSD-2-Clause), Zydis + Zycore (MIT), stb_image_write (MIT/public domain), renderdoc_app.h (MIT), the AMD FSR SDK 2.4.0 C API headers (MIT; AMD's signed FidelityFX DLLs are not included and are loaded only if the user places them next to the runtime). Boost (ICL, Asio; header-only) is used under BSL-1.0. glslang's `glslangValidator` (BSD-3-Clause) is a build tool and is not shipped. The full license texts of these components are those in the upstream trees under `gpu/vendor/`; they must accompany any distribution of a binary that links them.

The Windows audio path uses WASAPI (shared mode) and the video path uses the Vulkan loader supplied by the GPU driver; neither is redistributed. Because this runtime links the GPL-2.0-or-later renderer, the combined runtime executable is GPL-2.0-or-later. No game code, game asset, key or firmware is included.
