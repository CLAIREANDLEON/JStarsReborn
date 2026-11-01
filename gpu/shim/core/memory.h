// JStarsReborn: the MemoryManager surface used by the video core and GnmDriver.
// The runtime (runtime/jstars_memory.c) owns guest mappings; guest addresses are host addresses.
// Map/unmap notifications arrive through jgpu_mem_map/jgpu_mem_unmap (gpu/jgpu.h).
#pragma once
#include "common/singleton.h"
#include "common/types.h"
#include "core/address_space.h"

namespace Vulkan { class Rasterizer; }

namespace Core {
class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    AddressSpace& GetAddressSpace() { return impl; }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        return virtual_addr + size < 0x100'0000'0000ULL;
    }
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    /// Copies guest memory, zero-filling bytes that are not mapped.
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    /// Writes guest memory on behalf of the GPU. Returns false when the range has no separate
    /// unprotected view (the caller then writes through the guest mapping).
    bool TryWriteBacking(void* address, const void* data, u64 size);
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
using Memory = Common::Singleton<MemoryManager>;
} // namespace Core
