#!/usr/bin/env sh
# Builds and runs the platform-independent test suite (SHA-256, PARAM.SFO parser,
# supported-build manifest, game-folder validation) with AddressSanitizer and
# UndefinedBehaviorSanitizer on any POSIX host with a C++20 compiler.
# Usage: tools/portable_tests.sh [build-dir]   (CXX overrides the compiler)
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/build/portable"}
cxx=${CXX:-g++}
mkdir -p "$out"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$root/include" \
  "$root/src/setup/sha256.cpp" "$root/src/setup/game_validate.cpp" "$root/tests/gamecheck_tests.cpp" \
  -o "$out/gamecheck_tests"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$root/include" "$root/src/core/paths.cpp" "$root/tests/paths_tests.cpp" -o "$out/paths_tests"
(cd "$out" && TMPDIR="$out" ./paths_tests)
# Cross-check the portable SHA-256 against a digest recorded independently (BCrypt /
# Python hashlib) in setup-payload.txt, when the shipped runtime is present.
exe="$root/JStarsReborn.exe"
expected=$(awk -F '\t' '$3=="JStarsReborn.exe"{print $1}' "$root/setup-payload.txt" 2>/dev/null || true)
json="$out/sample-validation.json"
rm -f "$json"
if [ -f "$exe" ] && [ -n "$expected" ]; then
  (cd "$out" && TMPDIR="$out" REBORN_JSON_OUT="$json" ./gamecheck_tests "$exe" "$expected")
else
  echo "NOTE: shipped JStarsReborn.exe or setup-payload.txt absent; real-file SHA-256 cross-check skipped"
  (cd "$out" && TMPDIR="$out" REBORN_JSON_OUT="$json" ./gamecheck_tests)
fi
if command -v python3 >/dev/null 2>&1; then
  python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1],encoding="utf-8")); assert d["schema"]=="reborn.game-validation.v1" and d["game_import_supported"] is False; print("JSON report parsed by Python json:", sys.argv[1])' "$json"
else
  echo "NOTE: python3 absent; validation JSON was checked by substring only"
fi
