#!/usr/bin/env bash
# Builds the JStarsReborn release folder and ZIP (Windows x64, cross-compiled on Linux with mingw-w64).
#   tools/build_release.sh <gpu-lib.a> [out-dir]
# <gpu-lib.a>: libjstars_gpu.a built from gpu/ for Windows (see docs/BUILD_RELEASE.md).
# Output: <out-dir>/JStarsReborn-<version>/ and <out-dir>/JStarsReborn-<version>-win64.zip. Nothing is published.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
gpu_lib=$(realpath "${1:?path to the Windows libjstars_gpu.a}")
out=$(realpath -m "${2:-$here/build/release}")
version=$(sed -n 's/.*launcher_version = "\([0-9.]*\)".*/\1/p' "$here/src/launcher/launcher_core.hpp")
rt_version=$(sed -n 's/#define JSTARS_RUNTIME_VERSION "\(.*\)"/\1/p' "$here/runtime/jstars_rt.h")
[ "$version" = "$rt_version" ] || { echo "launcher ($version) and runtime ($rt_version) versions differ" >&2; exit 1; }
rel=$out/JStarsReborn-$version
rm -rf "$rel" "$out/JStarsReborn-$version-win64.zip"
mkdir -p "$rel/runtime" "$rel/LICENSES/third-party" "$rel/source"

echo "== launcher"
"$here/tools/build_launcher.sh" >/dev/null
cp "$here/build/launcher/JStarsReborn.exe" "$rel/JStarsReborn.exe"
echo "== runtime"
make -C "$here/runtime" jstars-boot-gpu.exe GPU_LIB_WIN="$gpu_lib" >/dev/null 2>"$out/runtime-link.log" || { grep -E "error|undefined" "$out/runtime-link.log" | head; exit 1; }
cp "$here/runtime/jstars-boot-gpu.exe" "$rel/runtime/jstars-runtime.exe"
cp "$here/packaging/runtime-README.txt" "$rel/runtime/README.txt"

echo "== documents and licenses"
cp "$here/packaging/README.txt" "$rel/README.txt"
cp "$here/CHANGELOG.md" "$rel/CHANGELOG.md"
cp "$here/gpu/vendor/shadps4/LICENSE" "$rel/LICENSES/GPL-2.0.txt"
cp "$here/THIRD_PARTY_NOTICES.md" "$rel/LICENSES/THIRD_PARTY_NOTICES.md"
cp "$here/licenses/"mingw-w64-*.txt "$rel/LICENSES/third-party/"
cp /usr/share/doc/gcc-13-base/copyright "$rel/LICENSES/third-party/gcc-runtime-libraries-copyright.txt"
cp "$here/packaging/BSL-1.0.txt" "$rel/LICENSES/third-party/boost-BSL-1.0.txt"
v=$here/gpu/vendor/externals
for pair in fmt/LICENSE:fmt half/LICENSE.txt:half magic_enum/LICENSE:magic_enum robin-map/LICENSE:robin-map sirit/LICENSE.txt:sirit \
            sirit/externals/SPIRV-Headers/LICENSE:SPIRV-Headers vma/LICENSE.txt:VulkanMemoryAllocator vulkan-headers/LICENSE.md:Vulkan-Headers \
            xxhash/LICENSE:xxHash zydis/LICENSE:Zydis zydis/dependencies/zycore/LICENSE:Zycore; do
    cp "$v/${pair%%:*}" "$rel/LICENSES/third-party/${pair##*:}-LICENSE.txt"
done
# xbyak ships a COPYRIGHT file; stb_image_write and renderdoc_app.h carry their license in the source file
cp "$v/xbyak/COPYRIGHT" "$rel/LICENSES/third-party/xbyak-LICENSE.txt"
sed -n '/^This software is available under 2 licenses/,$p' "$v/stb/stb_image_write.h" | head -45 > "$rel/LICENSES/third-party/stb-LICENSE.txt"
sed -n '1,25p' "$v/renderdoc/renderdoc_app.h" > "$rel/LICENSES/third-party/renderdoc_app-LICENSE.txt" || true

echo "== source"
tar -C "$here/.." --exclude='JStarsReborn/build' --exclude='JStarsReborn/runtime/*.exe' --exclude='JStarsReborn/runtime/jstars-boot' \
    --exclude='JStarsReborn/runtime/jstars-boot-gpu' --exclude='JStarsReborn/runtime/*.log' --exclude='JStarsReborn/runtime/*.selftest' \
    --exclude='JStarsReborn/runtime/obj*' --exclude='JStarsReborn/JStarsReborn.exe' --exclude='JStarsReborn/JStarsSetup.exe' \
    --exclude='JStarsReborn/tools/vkprobe/vkprobe.exe' --exclude='JStarsReborn/evidence' --exclude='*/__pycache__' \
    --exclude='JStarsReborn/gpu/build*' -czf "$rel/source/JStarsReborn-$version-source.tar.gz" JStarsReborn
cp "$here/docs/BUILD_RELEASE.md" "$rel/source/BUILD.md"

echo "== version and checksums"
{
    echo "JStarsReborn $version"
    echo "launcher: JStarsReborn.exe $version"
    echo "runtime:  runtime/jstars-runtime.exe $rt_version"
    echo "renderer: shadPS4 b4e7ae776071ceb1dd35004577a8719481e1d258 + gpu/patches/shadps4-local.patch"
    echo "supported game build: CUSA01593 version 01.02 (eboot.bin sha256 d43b7b5db10b5808c06691b27ef0ce1b5d27da6b1e07d811db5bea125ceaed54)"
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ) with $(x86_64-w64-mingw32-g++-posix --version | head -1); four gpu units with $(clang++-18 --version | head -1)"
    tmpd=$(mktemp -d); tar -xzf "$rel/source/JStarsReborn-$version-source.tar.gz" -C "$tmpd"
    echo "source tree digest: $(cd "$tmpd/JStarsReborn" && find . -type f | LC_ALL=C sort | xargs -d '\n' sha256sum | sha256sum | cut -c1-16) (sha256 of the sorted per-file sha256 list of the source archive)"
    rm -rf "$tmpd"
    echo "unpublished build: not a release; nothing has been published"
} > "$rel/VERSION.txt"
(cd "$rel" && find . -type f ! -name SHA256SUMS.txt | LC_ALL=C sort | sed 's#^\./##' | xargs -d '\n' sha256sum > SHA256SUMS.txt)
(cd "$out" && python3 - "$version" <<'PY'
import os, sys, zipfile
v = sys.argv[1]; root = f'JStarsReborn-{v}'
with zipfile.ZipFile(f'{root}-win64.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in os.walk(root):
        for f in sorted(files):
            p = os.path.join(d, f); z.write(p, p)
PY
)
(cd "$out" && sha256sum "JStarsReborn-$version-win64.zip" > "JStarsReborn-$version-win64.zip.sha256")
echo "== done"; du -sh "$rel" "$out/JStarsReborn-$version-win64.zip"; cat "$rel/VERSION.txt"
