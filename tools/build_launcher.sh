#!/usr/bin/env bash
# Builds the Windows launcher JStarsReborn.exe with mingw-w64 (static; only system DLLs). Output: build/launcher/JStarsReborn.exe
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); out=$here/build/launcher; mkdir -p "$out"
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}; WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
(cd "$here/resources" && $WINDRES -O coff launcher.rc -o "$out/launcher.res")
$CXX -std=c++20 -O2 -Wall -Wextra -municode -mwindows -static -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 \
  -I"$here/include" -I"$here/src/prepare" -I"$here/src/launcher" \
  "$here/src/launcher/main.cpp" "$here/src/launcher/launcher_core.cpp" "$here/src/prepare/prepare.cpp" \
  "$here/src/setup/sha256.cpp" "$here/src/setup/game_validate.cpp" "$here/src/core/paths.cpp" "$out/launcher.res" \
  -Wl,--no-insert-timestamp -o "$out/JStarsReborn.exe" -lcomctl32 -lole32 -loleaut32 -lshell32 -lshlwapi -luuid -lwindowscodecs -lmsimg32 -lgdi32 -luser32
sha256sum "$out/JStarsReborn.exe"
