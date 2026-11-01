#!/usr/bin/env bash
# Regenerates gpu/patches/shadps4-local.patch: unified diffs of every vendored shadPS4 file that differs from the
# pinned upstream commit (b4e7ae776071ceb1dd35004577a8719481e1d258).
# usage: tools/make_patches.sh <path to a shadPS4 checkout at the pinned commit>
set -eu
UP=${1:?path to a shadPS4 checkout at b4e7ae77}
HERE=$(cd "$(dirname "$0")/.." && pwd)
V=$HERE/gpu/vendor/shadps4
test "$(git -C "$UP" rev-parse HEAD)" = b4e7ae776071ceb1dd35004577a8719481e1d258 || { echo "checkout is not at the pinned commit" >&2; exit 1; }
OUT=$HERE/gpu/patches/shadps4-local.patch
: > "$OUT"
( cd "$V" && find src -type f | LC_ALL=C sort ) | while read -r f; do
    if [ -f "$UP/$f" ]; then
        if ! cmp -s "$UP/$f" "$V/$f"; then diff -u --label "a/$f" --label "b/$f" "$UP/$f" "$V/$f" >> "$OUT" || true; fi
    else
        echo "# file not in upstream at the pinned commit (ours): $f" >&2
    fi
done
grep -c '^+++ ' "$OUT" | sed 's/^/files patched: /'
