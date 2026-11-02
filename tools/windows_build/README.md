# Native Windows runtime build

`build_runtime.py` compiles the runtime with llvm-mingw on Windows using Python
3.11 or newer. It reads the C source list from `runtime/Makefile`, needs no make
or Unix shell, and keeps objects, logs and a hashed build manifest outside the
source tree. Use a new output directory for every build.

PowerShell (replace the tool paths with your installed locations):

```powershell
python tools/windows_build/build_runtime.py `
  --llvm-bin C:/toolchains/llvm-mingw/bin `
  --out-dir C:/builds/jstars-native-host-001
& C:/builds/jstars-native-host-001/jstars-boot-native.exe `
  --selftest-host --log C:/builds/jstars-native-host-001/selftest-host.log
$LASTEXITCODE
```

The default executable tests host services without a game or renderer. Verified
on the clean `656e15c7de8e27d80326afd06557acac24031e8b` export with llvm-mingw
20261020 UCRT x86_64, Clang 23.1.4: **266 checks, 0 failed** on Windows with local
UDP loopback access. A restricted preliminary run failed four UDP receive/address
checks; both records are retained in the shared handoff evidence. The existing
compiler warnings are retained in `compile.log`. No runtime C source was changed.

## Renderer compatibility

`--gpu-library C:/builds/renderer/libjstars_gpu.a` enables GPU compilation and
linking, but requires a renderer built against the same C++ runtime as llvm-mingw
(libc++). This option records a failed link and returns nonzero when incompatible.

The renderer in Claude's first source export, SHA-256
`4a41d95b5cb4f51ff9d1d71bf6d782930ee99f18a21c51b35f267e0681abf230`, was built with
GCC's libstdc++. All 19 GPU-enabled runtime C files compile with llvm-mingw, but
linking that archive fails with unresolved `std::__cxx11`, `std::__throw_*`, and
other libstdc++ symbols. The llvm-mingw libc++ distribution cannot directly replace
that dependency. A successful GPU build and gameplay equivalence are **not yet
verified**. Continue using the established Linux GCC/MinGW cross-build for game
probes until a compatible native renderer/toolchain is provided and tested.

The manifest's `build_succeeded` means only that compilation/linking completed.
Self-tests and game runs are separate evidence; the script never launches a game.
