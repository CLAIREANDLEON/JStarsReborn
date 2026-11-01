// JStarsReborn: the address-space operations the video core needs, backed by the runtime's host
// memory layer (guest addresses are host addresses). Interface from shadPS4 b4e7ae77 core/address_space.h.
#pragma once
#include <boost/icl/interval_set.hpp>
#include "common/enum.h"
#include "common/types.h"

namespace Core {
enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

class AddressSpace {
public:
    /// Changes host page protection for page tracking. Implemented in jgpu_glue.cpp.
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);
    /// Virtual range the console reserves for the system (tessellation factor ring buffer lives at its end).
    VAddr SystemReservedVirtualBase() const { return 0x7'0000'0000ULL; }
    u64 SystemReservedVirtualSize() const { return 0x1'0000'0000ULL; }
    /// Only used by the Linux userfaultfd tracker, which this build does not enable.
    boost::icl::interval_set<VAddr> GetUsableRegions() { return {}; }
};
} // namespace Core
