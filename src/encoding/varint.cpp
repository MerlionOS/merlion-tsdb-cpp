#include "merlion_tsdb/encoding/varint.hpp"

#include <cassert>

namespace merlion_tsdb::varint {

std::size_t put_uvarint(std::span<std::uint8_t> buf, std::uint64_t x) noexcept {
    assert(buf.size() >= max_varint_len64);
    std::size_t i = 0;
    while (x >= 0x80) {
        buf[i++] = static_cast<std::uint8_t>(x) | 0x80;
        x >>= 7;
    }
    buf[i++] = static_cast<std::uint8_t>(x);
    return i;
}

std::size_t put_varint(std::span<std::uint8_t> buf, std::int64_t x) noexcept {
    // Zigzag: (x << 1) ^ (x >> 63). Mapping non-negative n to 2n and -n to 2n-1
    // keeps small-magnitude integers small after the LEB128 encoding.
    const auto ux = static_cast<std::uint64_t>(x) << 1;
    const auto sign = static_cast<std::uint64_t>(x >> 63);  // arithmetic shift
    return put_uvarint(buf, ux ^ sign);
}

std::expected<std::pair<std::uint64_t, std::size_t>, encoding::ReadError>
read_uvarint(std::span<const std::uint8_t> buf) noexcept {
    if (buf.empty()) {
        return std::unexpected(encoding::ReadError::EndOfStream);
    }
    std::uint64_t x = 0;
    unsigned s = 0;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const std::uint8_t b = buf[i];
        if (i == max_varint_len64) {
            return std::unexpected(encoding::ReadError::VarintOverflow);
        }
        if (b < 0x80) {
            // Final byte must not have a non-canonical representation
            // (matches Go's check at the 10th byte for uint64 overflow).
            if (i == max_varint_len64 - 1 && b > 1) {
                return std::unexpected(encoding::ReadError::VarintOverflow);
            }
            return std::pair{x | (static_cast<std::uint64_t>(b) << s), i + 1};
        }
        x |= static_cast<std::uint64_t>(b & 0x7F) << s;
        s += 7;
    }
    return std::unexpected(encoding::ReadError::UnexpectedEnd);
}

std::expected<std::pair<std::int64_t, std::size_t>, encoding::ReadError>
read_varint(std::span<const std::uint8_t> buf) noexcept {
    auto r = read_uvarint(buf);
    if (!r) return std::unexpected(r.error());
    const std::uint64_t ux = r->first;
    // Reverse zigzag.
    std::int64_t x = static_cast<std::int64_t>(ux >> 1);
    if ((ux & 1) != 0) x = ~x;
    return std::pair{x, r->second};
}

}  // namespace merlion_tsdb::varint
