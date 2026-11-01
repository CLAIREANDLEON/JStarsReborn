#!/bin/bash
# Syntax-check every GPU library source with the host GCC (fast feedback; no objects written).
cd "$(dirname "$0")/.."
source tools/flags.sh
SRC=${1:-sources.txt}
mkdir -p /tmp/jgpu-syn
check() { f=$1; o=/tmp/jgpu-syn/$(echo $f | tr '/' '_').log
  g++ -std=c++23 -fsyntax-only -w -DNOMINMAX $INC $DEFS $f > $o 2>&1 && echo "OK   $f" || echo "FAIL $f"; }
export -f check; export INC DEFS
grep -v '^#' $SRC | xargs -P2 -I{} bash -c 'check {}'
