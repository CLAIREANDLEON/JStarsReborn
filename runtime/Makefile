# Linux development harness for the execution runtime (System V ABI is native there).
# The Windows host layer is a separate port of jstars_loader.c's OS calls (see docs/ADR-005).
CC ?= gcc
CFLAGS ?= -std=gnu2x -O2 -g -Wall -Wextra
SRC = jstars_loader.c jstars_net.c jstars_rooms.c jstars_rt.c jstars_kernel.c jstars_memory.c jstars_system.c jstars_input.c jstars_av.c jstars_os.c jstars_gpu_host.c jstars_thunk.c jstars_audit.c jstars_selftest.c jstars_fs.c jstars_evf.c jstars_platform.c jstars_savedata.c jstars_avplayer.c jstars_audiohost.c
jstars-boot: $(SRC) jstars_rt.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lpthread -lm
clean:
	rm -f jstars-boot

# Windows x64 build (static, winpthreads). Cross-compiled here; run under Wine for structural checks.
# Wine is not Windows: only a run on a Windows machine counts as Windows evidence.
jstars-boot.exe: $(SRC) jstars_rt.h jstars_os.h
	x86_64-w64-mingw32-gcc-posix $(CFLAGS) -static -o $@ $(SRC) -lws2_32 -lole32 -luser32 -ldbghelp -lshell32 -lpthread -lm

# ---- GPU-linked runtime (checkpoint 0.3.x): the shadPS4-derived renderer in ../gpu is linked in --------
# GPU_LIB points at libjstars_gpu.a built by `cmake -S ../gpu -B build-gpu && cmake --build build-gpu`.
# GPL-2.0-or-later as a whole (see ../gpu/VENDOR.md). Without -DJSTARS_GPU the hooks are no-ops.
GPU_LIB ?= ../gpu/build/libjstars_gpu.a
CXX ?= g++
jstars-boot-gpu: $(SRC) jstars_rt.h $(GPU_LIB)
	$(CC) $(CFLAGS) -DJSTARS_GPU -I../gpu -c $(filter %.c,$(SRC)) && \
	$(CXX) -o $@ $(patsubst %.c,%.o,$(filter %.c,$(SRC))) $(GPU_LIB) -lX11 -lvulkan -ldl -lpthread -lm
	rm -f $(patsubst %.c,%.o,$(filter %.c,$(SRC)))

# Windows x64, GPU-linked (cross-compiled with mingw-w64; GPL-2.0-or-later as a whole). The Vulkan loader
# (vulkan-1.dll, installed by the GPU driver) is loaded at run time, so no import library is needed.
GPU_LIB_WIN ?= ../gpu/build-win/libjstars_gpu.a
WIN_CC ?= x86_64-w64-mingw32-gcc-posix
WIN_CXX ?= x86_64-w64-mingw32-g++-posix
# --allow-multiple-definition: clang- and GCC-built objects emit the same C++ inline/template statics (guard variables,
# Singleton instances) in differently-typed COMDAT sections, which ld reports as duplicates; all 15 are of that kind.
jstars-boot-gpu.exe: $(SRC) jstars_rt.h jstars_os.h $(GPU_LIB_WIN)
	mkdir -p obj-win && $(WIN_CC) $(CFLAGS) -DJSTARS_GPU -I../gpu -c $(filter %.c,$(SRC)) && mv *.o obj-win/ && \
	$(WIN_CXX) -static -static-libgcc -static-libstdc++ -Wl,--allow-multiple-definition -o $@ $(addprefix obj-win/,$(patsubst %.c,%.o,$(filter %.c,$(SRC)))) $(GPU_LIB_WIN) \
	  -lws2_32 -lmswsock -luser32 -lgdi32 -ldbghelp -lbcrypt -lole32 -lshell32 -lshlwapi -ladvapi32 -lwinmm -lntdll -lpthread
	rm -rf obj-win

# Diagnostic variant: every malloc/calloc/realloc/free of the whole program goes through the guard-page allocator
# (jstars_heapguard.c). For short runs only (--stop-at-first-gnm, a few seconds of the game); never a performance build.
jstars-boot-gpu-hg.exe: $(SRC) jstars_heapguard.c jstars_rt.h jstars_os.h $(GPU_LIB_WIN)
	mkdir -p obj-win-hg && $(WIN_CC) $(CFLAGS) -DJSTARS_GPU -DJSTARS_HEAPGUARD -I../gpu -c $(filter %.c,$(SRC)) jstars_heapguard.c && mv *.o obj-win-hg/ && \
	$(WIN_CXX) -static -static-libgcc -static-libstdc++ -Wl,--allow-multiple-definition -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -o $@ $(addprefix obj-win-hg/,$(patsubst %.c,%.o,$(filter %.c,$(SRC)) jstars_heapguard.c)) $(GPU_LIB_WIN) \
	  -lws2_32 -lmswsock -luser32 -lgdi32 -ldbghelp -lbcrypt -lole32 -lshell32 -lshlwapi -ladvapi32 -lwinmm -lntdll -lpthread
	rm -rf obj-win-hg
