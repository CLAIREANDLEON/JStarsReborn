#!/usr/bin/env bash
# Regenerates gpu/patches/shadps4-local.patch: every difference between the vendored shadPS4 files
# (gpu/vendor/shadps4/src) and the pristine upstream checkout at the pinned revision. Only files that
# exist in the vendored tree are compared. Usage: tools/make_patches.sh /path/to/shadPS4-checkout
set -euo pipefail
UP=${1:?path to a shadPS4 checkout at the pinned revision}
HERE=$(cd "$(dirname "$0")/.." && pwd)
PIN=$(grep -oE 'b4e7ae77[0-9a-f]*' "$HERE/VENDOR.md" | head -1)
HEAD=$(git -C "$UP" rev-parse HEAD)
case "$HEAD" in "$PIN"*) ;; *) echo "checkout is at $HEAD, expected $PIN" >&2; exit 1;; esac
OUT="$HERE/patches/shadps4-local.patch"
: > "$OUT"
cd "$HERE/vendor/shadps4"
find src -type f | LC_ALL=C sort | while read -r f; do
  if [ -f "$UP/$f" ]; then
    cmp -s "$UP/$f" "$f" || diff -u --label "a/$f" --label "b/$f" "$UP/$f" "$f" >> "$OUT" || true
  else
    echo "# vendored file not in upstream: $f" >&2
  fi
done
echo "wrote $OUT ($(grep -c '^--- a/' "$OUT") files differ, $(wc -l < "$OUT") lines)"
