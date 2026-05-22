#include "merlion_tsdb/encoding/bstream.hpp"

#include <cassert>
#include <cstring>

#include "merlion_tsdb/encoding/varint.hpp"

namespace merlion_tsdb::encoding {

// --- BitWriter ---------------------------------------------------------------

void BitWriter::write_bit(bool b) noexcept {
    if (count_ == 0) {
        stream_.push_back(0);
        count_ = 8;
    }
    if (b) {
        stream_.back() |= static_cast<std::uint8_t>(1U << (count_ - 1));
    }
    --count_;
}

void BitWriter::write_byte(std::uint8_t byt) noexcept {
    if (count_ == 0) {
        stream_.push_back(byt);
        return;
    }
    // Fill the remaining `count_` bits of the current byte with the high bits
    // of `byt`, then start a new byte containing the rest. `count_` does not
    // change because we wrote exactly 8 bits.
    stream_.back() |= static_cast<std::uint8_t>(byt >> (8U - count_));
    stream_.push_back(static_cast<std::uint8_t>(byt << count_));
}

void BitWriter::write_bits(std::uint64_t u, int nbits) noexcept {
    assert(nbits >= 0 && nbits <= 64);
    if (nbits == 0) return;  // Shift by 64 would be UB; nothing to write anyway.
    // Left-align the bits we want to write, then shift out one byte / one bit
    // at a time, MSB-first. Mirrors tsdb/chunkenc/bstream.go writeBits.
    u <<= static_cast<unsigned>(64 - nbits);
    while (nbits >= 8) {
        write_byte(static_cast<std::uint8_t>(u >> 56));
        u <<= 8;
        nbits -= 8;
    }
    while (nbits > 0) {
        write_bit((u >> 63) != 0);
        u <<= 1;
        --nbits;
    }
}

// --- BitReader ---------------------------------------------------------------

BitReader::BitReader(std::span<const std::uint8_t> bytes) noexcept
    : stream_(bytes) {
    if (!stream_.empty()) {
        last_ = stream_.back();
    }
}

std::expected<bool, ReadError> BitReader::read_bit() noexcept {
    if (valid_ == 0) {
        if (!load_next_buffer(1)) {
            return std::unexpected(ReadError::EndOfStream);
        }
    }
    --valid_;
    const std::uint64_t mask = std::uint64_t{1} << valid_;
    return (buffer_ & mask) != 0;
}

std::expected<std::uint8_t, ReadError> BitReader::read_byte() noexcept {
    auto v = read_bits(8);
    if (!v) return std::unexpected(v.error());
    return static_cast<std::uint8_t>(*v);
}

std::expected<std::uint64_t, ReadError> BitReader::read_bits(std::uint8_t nbits) noexcept {
    assert(nbits <= 64);
    if (nbits == 0) return std::uint64_t{0};

    if (valid_ == 0) {
        if (!load_next_buffer(nbits)) {
            return std::unexpected(ReadError::EndOfStream);
        }
    }

    if (nbits <= valid_) {
        // Special case nbits == 64: `1 << 64` is UB, so build all-ones directly.
        const std::uint64_t mask =
            nbits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << nbits) - 1;
        valid_ = static_cast<std::uint8_t>(valid_ - nbits);
        return (buffer_ >> valid_) & mask;
    }

    // Need bits from two consecutive buffer loads.
    const std::uint64_t low_mask = (std::uint64_t{1} << valid_) - 1;
    const std::uint8_t  remaining = static_cast<std::uint8_t>(nbits - valid_);
    std::uint64_t v = (buffer_ & low_mask) << remaining;
    valid_ = 0;

    if (!load_next_buffer(remaining)) {
        return std::unexpected(ReadError::EndOfStream);
    }
    const std::uint64_t hi_mask = (std::uint64_t{1} << remaining) - 1;
    v |= (buffer_ >> (valid_ - remaining)) & hi_mask;
    valid_ = static_cast<std::uint8_t>(valid_ - remaining);
    return v;
}

std::expected<std::uint64_t, ReadError> BitReader::read_uvarint() noexcept {
    std::uint64_t x = 0;
    unsigned s = 0;
    for (int i = 0; i < varint::max_varint_len64; ++i) {
        auto b = read_byte();
        if (!b) return std::unexpected(b.error());
        if (*b < 0x80) {
            if (i == varint::max_varint_len64 - 1 && *b > 1) {
                return std::unexpected(ReadError::VarintOverflow);
            }
            return x | (static_cast<std::uint64_t>(*b) << s);
        }
        x |= static_cast<std::uint64_t>(*b & 0x7F) << s;
        s += 7;
    }
    return std::unexpected(ReadError::VarintOverflow);
}

std::expected<std::int64_t, ReadError> BitReader::read_varint() noexcept {
    auto ux = read_uvarint();
    if (!ux) return std::unexpected(ux.error());
    std::int64_t x = static_cast<std::int64_t>(*ux >> 1);
    if ((*ux & 1U) != 0) x = ~x;
    return x;
}

bool BitReader::load_next_buffer(std::uint8_t nbits) noexcept {
    if (stream_offset_ >= stream_.size()) {
        return false;
    }

    // Fast path: at least 8 bytes ahead of the tail. Reading the very last
    // byte is left to the slow path so that concurrent writers updating it
    // can't be observed mid-flight.
    if (stream_offset_ + 8 < stream_.size()) {
        std::uint64_t b;
        std::memcpy(&b, stream_.data() + stream_offset_, sizeof(b));
        // Convert big-endian on-disk bytes to host order MSB-first packing.
        b = (b & 0x00000000000000FFULL) << 56 |
            (b & 0x000000000000FF00ULL) << 40 |
            (b & 0x0000000000FF0000ULL) << 24 |
            (b & 0x00000000FF000000ULL) << 8  |
            (b & 0x000000FF00000000ULL) >> 8  |
            (b & 0x0000FF0000000000ULL) >> 24 |
            (b & 0x00FF000000000000ULL) >> 40 |
            (b & 0xFF00000000000000ULL) >> 56;
        buffer_ = b;
        stream_offset_ += 8;
        valid_ = 64;
        return true;
    }

    // Slow path: <= 8 bytes left. Read them and pack into the buffer MSB-first.
    int nbytes = (nbits / 8) + 1;
    const int remaining = static_cast<int>(stream_.size() - stream_offset_);
    if (nbytes > remaining) nbytes = remaining;

    std::uint64_t b = 0;
    int skip = 0;
    if (stream_offset_ + static_cast<std::size_t>(nbytes) == stream_.size()) {
        // Use our cached copy of the tail byte (Go does this to avoid races
        // with concurrent appenders; we keep the behaviour for safety).
        b |= static_cast<std::uint64_t>(last_);
        skip = 1;
    }

    for (int i = 0; i < nbytes - skip; ++i) {
        b |= static_cast<std::uint64_t>(stream_[stream_offset_ + static_cast<std::size_t>(i)])
             << static_cast<unsigned>(8 * (nbytes - i - 1));
    }

    buffer_ = b;
    stream_offset_ += static_cast<std::size_t>(nbytes);
    valid_ = static_cast<std::uint8_t>(nbytes * 8);
    return true;
}

}  // namespace merlion_tsdb::encoding
