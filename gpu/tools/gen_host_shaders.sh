#!/bin/bash
# Generates the host shader headers (video_core/host_shaders/*.h) with glslangValidator, mirroring
# vendor/shadps4/src/video_core/host_shaders/CMakeLists.txt. Usage: gen_host_shaders.sh <out-include-dir>
set -e
GL=${GLSLANG:-glslangValidator}
SRC="$(cd "$(dirname "$0")/.." && pwd)/vendor/shadps4/src/video_core/host_shaders"
OUT=$1/video_core/host_shaders
mkdir -p "$OUT"
add() { # file name [defines...]
  local f=$1 n=$2; shift 2
  local sym; sym=$(echo "$n" | tr 'a-z' 'A-Z')
  local defs=(); for d in "$@"; do defs+=("-D$d"); done
  "$GL" --target-env vulkan1.3 -o "$OUT/$n.h" --vn "$sym" "${defs[@]}" "$SRC/$f" > /dev/null
  sed -i '1i #include <cstdint>' "$OUT/$n.h"  # same post-processing as AddShaderHeaderInclude.cmake
}
add color_to_ms_depth.frag color_to_ms_depth_frag
add ms_image_blit.frag ms_image_blit_frag
add ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1
add fault_buffer_process.comp fault_buffer_process_comp
add fs_tri.vert fs_tri_vert
add fsr.comp fsr_easu_comp SAMPLE_EASU=1
add fsr.comp fsr_rcas_comp SAMPLE_RCAS=1
add post_process.frag post_process_frag
for bpp in 8 16 32 64 96 128; do
  add tiling.comp tiling_micro_${bpp}_comp BITS_PER_PIXEL=$bpp IS_MACRO_TILED=0
  add tiling.comp tiling_macro_${bpp}_comp BITS_PER_PIXEL=$bpp IS_MACRO_TILED=1
done
echo "host shaders: $(ls "$OUT" | wc -l) headers in $OUT"
