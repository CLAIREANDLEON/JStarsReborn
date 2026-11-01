#!/usr/bin/env bash
# Execution-milestone smoke test. Reads the game folder (never writes to it), prepares and links the
# image into a work directory keyed by title, version and eboot hash, builds the Linux harness, runs the
# original code and checks the log for the milestones reached. Exit status 0 means "module initializers
# ran and the entry point was entered"; the stop reason is printed either way.
#   tools/boot_smoke.sh <game-folder> <work-root> [path/to/aerolib.inl]
set -euo pipefail
game=${1:?game folder}; root=${2:?work root}; aerolib=${3:-}
here=$(cd "$(dirname "$0")/.." && pwd)
title=$(python3 - "$game/sce_sys/param.sfo" <<'PY'
import sys; sys.path.insert(0, sys.argv[0] and __import__('os').path.dirname(__import__('os').path.abspath(__file__)) if False else '.')
import struct
d = open(sys.argv[1], 'rb').read()
_, _, kt, dt, n = struct.unpack('<4sIIII', d[:20])
v = {}
for i in range(n):
    k, f, sz, cap, off = struct.unpack('<HHIII', d[20 + 16 * i:36 + 16 * i])
    key = d[kt + k:d.index(0, kt + k)].decode()
    v[key] = d[dt + off:dt + off + sz].rstrip(b'\0').decode('utf8', 'replace')
print(v['TITLE_ID'], v['APP_VER'])
PY
)
set -- $title; tid=$1; ver=$2
hash=$(sha256sum "$game/eboot.bin" | cut -c1-12)
work="$root/${tid}_${ver}_${hash}"
mkdir -p "$work"/{prepared,logs,reports}
echo "work store: $work"
cd "$here/tools/prepare"
python3 prepare.py "$game" --out "$work/prepared" | tee "$work/logs/prepare.log"
python3 link_modules.py "$game" --out "$work/prepared" | tee "$work/logs/link.log"
[ -n "$aerolib" ] && python3 nid_names.py "$work/prepared/link.json" "$aerolib" "$work/prepared/import-names.tsv"
make -C "$here/runtime" jstars-boot >/dev/null
set +e
timeout "${BOOT_TIMEOUT:-30}" "$here/runtime/jstars-boot" --boot "$work/prepared/boot-linked.bin" --log "$work/logs/boot.log" >/dev/null 2>&1
code=$?
set -e
echo "loader exit code: $code"
grep -q "module 0 initializer returned 0" "$work/logs/boot.log" && echo "PASS libc.prx initializer returned 0" || { echo "FAIL libc.prx initializer"; exit 1; }
grep -q "module 1 initializer returned 0" "$work/logs/boot.log" && echo "PASS libSceFios2.prx initializer returned 0" || { echo "FAIL Fios2 initializer"; exit 1; }
grep -q "entering original x86-64 code" "$work/logs/boot.log" && echo "PASS entry point entered" || { echo "FAIL entry"; exit 1; }
echo "stopped at: $(grep -E 'UNIMPLEMENTED|FAULT|guest called its exit' "$work/logs/boot.log" | tail -1)"
