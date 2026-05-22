#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "merlion_tsdb/encoding/error.hpp"

// Bit-level stream reader and writer.
//
// Bytes are written MSB-first within each byte, and the stream grows
// left-to-right. This layout is required for binary compatibility with
// Prometheus's tsdb/chunkenc/bstream.go (originally derived from
// github.com/dgryski/go-tsz). DO NOT change endianness or bit order.
namespace merlion_tsdb::encoding {

class BitWriter {
public:
    BitWriter() = default;

    // Adopts `stream` as the initial buffer. The next bit will be written
    // into a freshly appended byte (matching Go's count==0 semantics, which
    // assume the existing buffer is byte-aligned).
    explicit BitWriter(std::vector<std::uint8_t> stream) noexcept
        : stream_(std::move(stream)) {}

    void write_bit(bool b) noexcept;
    void write_byte(std::uint8_t byt) noexcept;

    // Writes the `nbits` right-most bits of `u` to the stream, MSB-first.
    // Requires 0 <= nbits <= 64.
    void write_bits(std::uint64_t u, int nbits) noexcept;

    // Read-only view of the stream so far.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {stream_.data(), stream_.size()};
    }

    // Mutable access for header rewrites (e.g., XORChunk's sample count
    // lives in the first two bytes and is updated after every append).
    [[nodiscard]] std::span<std::uint8_t> mutable_bytes() noexcept {
        return {stream_.data(), stream_.size()};
    }

    void reset(std::vector<std::uint8_t> stream = {}) noexcept {
        stream_ = std::move(stream);
        count_ = 0;
    }

    // Releases the underlying buffer to the caller. The writer becomes
    // empty afterwards.
    [[nodiscard]] std::vector<std::uint8_t> release() noexcept {
        auto out = std::move(stream_);
        stream_.clear();
        count_ = 0;
        return out;
    }

private:
    std::vector<std::uint8_t> stream_;
    // Number of right-most bits still available in the current (last) byte.
    // 0 means the last byte is fully used; the next write must append a new byte.
    std::uint8_t count_ = 0;
};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) noexcept;

    std::expected<bool, ReadError> read_bit() noexcept;
    std::expected<std::uint8_t, ReadError> read_byte() noexcept;

    // Reads the next `nbits` bits into the low-order bits of a uint64.
    // Requires 0 <= nbits <= 64.
    std::expected<std::uint64_t, ReadError> read_bits(std::uint8_t nbits) noexcept;

    // Convenience readers matching Go's chunkenc/bstream.go.
    std::expected<std::uint64_t, ReadError> read_uvarint() noexcept;
    std::expected<std::int64_t, ReadError>  read_varint() noexcept;

    // True when the reader has consumed all input and has no buffered bits.
    [[nodiscard]] bool at_end() const noexcept {
        return stream_offset_ >= stream_.size() && valid_ == 0;
    }

private:
    bool load_next_buffer(std::uint8_t nbits) noexcept;

    std::span<const std::uint8_t> stream_;
    std::size_t stream_offset_ = 0;  // next byte in stream_ to load into buffer_
    std::uint64_t buffer_ = 0;       // up to 8 buffered bytes in MSB order
    std::uint8_t valid_ = 0;         // number of valid right-most bits in buffer_
    std::uint8_t last_ = 0;          // copy of stream_.back() to avoid TOCTOU on the tail
};

}  // namespace merlion_tsdb::encoding
