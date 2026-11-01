#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace reborn {
// Portable, dependency-free SHA-256 (FIPS 180-4). The Windows-only setup modules use
// BCrypt; this implementation exists so parsers and validators that touch untrusted
// bytes can be built and run under sanitizers on any C++20 host. Correctness is
// established by NIST vectors plus the project's own shipped files (see tests).
class PortableSha256 {
public:
    PortableSha256() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    // Finalizes the digest. The object must not be updated or finished again.
    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;
    [[nodiscard]] static std::string to_hex(const std::array<std::uint8_t, 32>& digest);

private:
    void compress(const std::uint8_t* block) noexcept;
    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::uint64_t total_ = 0;
    std::size_t used_ = 0;
};

[[nodiscard]] std::string sha256_hex(const void* data, std::size_t size);
}
