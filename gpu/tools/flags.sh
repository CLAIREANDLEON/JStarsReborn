# Include directories shared by every build of the GPU library (relative to gpu/).
G="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V=$G/vendor
INC="-I$G/shim -I$V/shadps4 -I${JGPU_GEN:-/tmp/jgpu-gen} -I$V/externals/stb -I$V/externals/renderdoc -I$V/externals/zydis/include -I$V/externals/zydis/src -I$V/externals/zydis/dependencies/zycore/include -I$V/shadps4/src -I$V/externals/fmt/include -I$V/externals/vulkan-headers/include -I$V/externals/vma/include \
 -I$V/externals/sirit/include -I$V/externals/sirit/externals/SPIRV-Headers/include -I$V/externals/robin-map/include \
 -I$V/externals/xbyak -I$V/externals/magic_enum/include -I$V/externals/half/include -I$V/externals/gcn/include -I$V/externals/xxhash"
DEFS="-DFMT_HEADER_ONLY=0 -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -DVULKAN_HPP_NO_EXCEPTIONS -DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 -DVULKAN_HPP_NO_CONSTRUCTORS -DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS -DVK_NO_PROTOTYPES -DVMA_STATIC_VULKAN_FUNCTIONS=0 -DVMA_DYNAMIC_VULKAN_FUNCTIONS=0 -DXXH_INLINE_ALL"
