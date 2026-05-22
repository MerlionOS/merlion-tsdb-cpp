#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "merlion_tsdb/encoding/error.hpp"

// Variable-length integer encoding matching Go's encoding/binary:
//
//   uvarint  — LEB128: 7-bit groups, low-to-high, MSB=1 means more bytes follow.
//   varint   — zigzag-then-uvarint:  (x << 1) ^ (x >> 63) for int64 → uint64.
//
// Maximum uvarint64 length is 10 bytes (9 full + 1 with stop bit).
namespace merlion_tsdb::varint {

inline constexpr int max_varint_len64 = 10;

// Encodes x into buf. Returns the number of bytes written.
// Precondition: buf.size() >= max_varint_len64.
std::size_t put_uvarint(std::span<std::uint8_t> buf, std::uint64_t x) noexcept;
std::size_t put_varint(std::span<std::uint8_t> buf, std::int64_t x) noexcept;

// Decodes from buf. Returns (value, bytes consumed) on success.
// EndOfStream:    buf empty.
// UnexpectedEnd:  ran out mid-value (last byte still had continuation bit).
// VarintOverflow: more than max_varint_len64 bytes consumed.
std::expected<std::pair<std::uint64_t, std::size_t>, encoding::ReadError>
read_uvarint(std::span<const std::uint8_t> buf) noexcept;

std::expected<std::pair<std::int64_t, std::size_t>, encoding::ReadError>
read_varint(std::span<const std::uint8_t> buf) noexcept;

}  // namespace merlion_tsdb::varint
