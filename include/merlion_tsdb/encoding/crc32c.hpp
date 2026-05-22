#pragma once

#include <cstdint>
#include <span>

// CRC32-Castagnoli (polynomial 0x1EDC6F41, reversed 0x82F63B78).
//
// This is the variant Prometheus uses for WAL record framing — see
// `prometheus/tsdb/wlog/wlog.go:50` (`crc32.MakeTable(crc32.Castagnoli)`).
// NOT to be confused with the IEEE CRC32 used by zip / gzip / Ethernet.
//
// Standard invocation: `state = k_init`, fold each byte via `update`,
// then `finalize` (XOR with k_init) to produce the wire value.
namespace merlion_tsdb::crc32c {

inline constexpr std::uint32_t k_init = 0xFFFFFFFFU;

// CRC32C check value for the canonical "123456789" input. Exposed so
// callers (or tests) can assert their build is using the right polynomial.
inline constexpr std::uint32_t k_check = 0xE3069283U;

// Folds `data` into the running CRC state. Returns the new state.
[[nodiscard]] std::uint32_t update(std::uint32_t state,
                                   std::span<const std::uint8_t> data) noexcept;

// XOR-out step. Required to convert internal state into the on-wire value.
[[nodiscard]] constexpr std::uint32_t finalize(std::uint32_t state) noexcept {
    return state ^ k_init;
}

// Convenience: one-shot computation over a buffer.
[[nodiscard]] inline std::uint32_t compute(std::span<const std::uint8_t> data) noexcept {
    return finalize(update(k_init, data));
}

}  // namespace merlion_tsdb::crc32c
