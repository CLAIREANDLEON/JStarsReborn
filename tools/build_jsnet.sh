#!/usr/bin/env bash
# Builds gpu/jsnet (libjstars_net.a) for Windows with clang++ against mingw-w64 GCC 13's libstdc++.
# usage: tools/build_jsnet.sh <build dir> <deps prefix> [boost include dir]
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
B=${1:?build dir}; D=${2:?deps prefix}; BOOST=${3:-}
GXX=/usr/lib/gcc/x86_64-w64-mingw32/13-posix/include/c++
# -femulated-tls: mingw-w64 GCC (and its libstdc++, e.g. std::call_once) uses emulated TLS.
T="--target=x86_64-w64-windows-gnu --sysroot=/usr/x86_64-w64-mingw32 -femulated-tls"
cmake -S "$HERE/gpu/jsnet" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR=$(command -v x86_64-w64-mingw32-ar) \
  -DCMAKE_RANLIB=$(command -v x86_64-w64-mingw32-ranlib) \
  -DCMAKE_C_FLAGS="$T" -DCMAKE_CXX_FLAGS="$T -nostdinc++ -isystem $GXX -isystem $GXX/x86_64-w64-mingw32" \
  -DJSNET_DEPS="$D" -DJSNET_BOOST_INCLUDE="$BOOST" >/dev/null
ninja -C "$B" -k 0
