#!/usr/bin/env bash
# Checks that the native preparation (src/prepare, used by the launcher) produces byte-identical output to the
# Python reference tools for the same game folder. Usage: tools/prepare_native_check.sh <game-folder> <aerolib.inl>
# The game folder is only read; output goes to a temporary directory.
set -euo pipefail
game=${1:?game folder}; aerolib=${2:?shadPS4 aerolib.inl}
here=$(cd "$(dirname "$0")/.." && pwd); tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
g++ -std=c++20 -O2 -I"$here/include" -I"$here/src/prepare" "$here/src/prepare/prepare.cpp" "$here/src/prepare/prepare_cli.cpp" \
    "$here/src/setup/sha256.cpp" -o "$tmp/jstars-prepare"
(cd "$here/tools/prepare" && python3 prepare.py "$game" --out "$tmp/py" >/dev/null && python3 link_modules.py "$game" --out "$tmp/py" >/dev/null &&
 python3 nid_names.py "$tmp/py/link.json" "$aerolib" "$tmp/py/import-names.tsv" >/dev/null)
"$tmp/jstars-prepare" "$game" "$tmp/native" 2>/dev/null
cmp "$tmp/py/boot-linked.bin" "$tmp/native/boot-linked.bin"
cmp "$tmp/py/import-names.tsv" "$tmp/native/import-names.tsv"
sha256sum "$tmp/native/boot-linked.bin" "$tmp/native/import-names.tsv" | sed "s#$tmp/native/##"
echo "native preparation output is identical to the Python tools"
