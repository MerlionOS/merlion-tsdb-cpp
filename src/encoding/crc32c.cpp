#include "merlion_tsdb/encoding/crc32c.hpp"

#include <array>

namespace merlion_tsdb::crc32c {

namespace {

// Reversed Castagnoli polynomial (matches the canonical software CRC32C
// table used by Go's `crc32.MakeTable(crc32.Castagnoli)`).
constexpr std::uint32_t k_poly_reversed = 0x82F63B78U;

constexpr auto k_table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1U) ? (k_poly_reversed ^ (c >> 1U)) : (c >> 1U);
        }
        t[i] = c;
    }
    return t;
}();

}  // namespace

std::uint32_t update(std::uint32_t state,
                     std::span<const std::uint8_t> data) noexcept {
    // Byte-at-a-time slice-by-1 implementation. Adequate for MVP (WAL records
    // are small; throughput is dominated by syscalls). Hardware acceleration
    // (ARMv8 __crc32cb / Intel SSE4.2 _mm_crc32_u8) can replace this later
    // behind a uniform interface.
    for (std::uint8_t b : data) {
        state = k_table[(state ^ b) & 0xFFU] ^ (state >> 8U);
    }
    return state;
}

}  // namespace merlion_tsdb::crc32c
