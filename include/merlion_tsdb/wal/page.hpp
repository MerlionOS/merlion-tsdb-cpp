#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "merlion_tsdb/encoding/error.hpp"

// Page-level record framing for the WAL.
//
// Records are framed with a 7-byte header (`type | length-be-u16 | crc-be-u32`)
// and packed into fixed-size 32 KiB pages. A logical record longer than the
// remaining page room is split into recFirst + recMiddle* + recLast fragments,
// each with its own header and CRC. The last < 7 bytes of a page (if any) are
// zero-padded; the leading zero byte acts as recPageTerm and tells the reader
// to skip to the next page boundary.
//
// See SPEC.md §4 (WAL framing) and the Go reference at
// `prometheus/tsdb/wlog/wlog.go:40-50, 622-628`.
namespace merlion_tsdb::wal {

inline constexpr std::size_t k_page_size          = 32U * 1024U;
inline constexpr std::size_t k_record_header_size = 7;
inline constexpr std::size_t k_max_fragment_body  = k_page_size - k_record_header_size;

// Fragment-type values stored in the bottom 3 bits of the header type byte.
enum class RecType : std::uint8_t {
    PageTerm = 0,  // rest of page is zero padding; advance to next page
    Full     = 1,  // complete logical record fits in one fragment
    First    = 2,  // first fragment of a multi-fragment record
    Middle   = 3,  // interior fragment
    Last     = 4,  // final fragment
};

// Compression flags ORed into the type byte. Not used in the MVP framer
// (writes uncompressed only); reader honours them for forward compatibility.
inline constexpr std::uint8_t k_snappy_mask   = 1U << 3;  // 0x08
inline constexpr std::uint8_t k_zstd_mask     = 1U << 4;  // 0x10
inline constexpr std::uint8_t k_rec_type_mask = 0x07;

// In-memory writer accumulating framed records into a single byte buffer.
// Each call to log() may append zero or more fragment headers + bodies.
class PageWriter {
public:
    // Appends one logical record. The body is fragmented across page
    // boundaries as needed. Each fragment carries its own CRC over its body.
    void log(std::span<const std::uint8_t> body);

    // Zero-pads the current page out to the next page boundary, if any
    // bytes have been written into it. No-op if the buffer is already
    // aligned. Required before rolling to a new segment because records
    // do not span segments.
    void close_page();

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {buf_.data(), buf_.size()};
    }

    // Releases the underlying buffer to the caller.
    [[nodiscard]] std::vector<std::uint8_t> release() noexcept {
        auto out = std::move(buf_);
        buf_.clear();
        return out;
    }

private:
    void write_header(RecType t, std::uint16_t length, std::uint32_t crc);
    std::size_t page_offset() const noexcept { return buf_.size() % k_page_size; }
    std::size_t page_room_for_header() const noexcept;

    std::vector<std::uint8_t> buf_;
};

// Errors produced by the reader. We extend ReadError so callers using a
// single error category for "anything decoding-related" still work.
enum class WalReadError : std::uint8_t {
    EndOfStream      = 0,
    UnexpectedEnd    = 1,  // header started but body / crc truncated
    InvalidRecType   = 2,  // type byte's bottom 3 bits aren't in 0..4
    CrcMismatch      = 3,
    UnexpectedFirst  = 4,  // saw a First fragment while another record was open
    UnexpectedMiddle = 5,  // saw Middle/Last with no open record
    TornRecord       = 6,  // hit EOF mid-record (last fragment was First/Middle)
    UnsupportedCompression = 7,  // MVP reader rejects compressed records
};

// Reads framed records from an in-memory byte buffer (typically produced
// by PageWriter, or read from a segment file). Reassembles fragments into
// logical records and validates CRCs.
class PageReader {
public:
    explicit PageReader(std::span<const std::uint8_t> bytes) noexcept
        : buf_(bytes) {}

    // Returns the next logical record, EndOfStream when the buffer is
    // cleanly exhausted, or a WalReadError on corruption. The returned
    // span references an internally owned buffer that is valid until the
    // next next() call.
    std::expected<std::span<const std::uint8_t>, WalReadError> next();

    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return offset_; }

private:
    std::expected<void, WalReadError> read_fragment_header(
        RecType& out_type, std::uint16_t& out_len, std::uint32_t& out_crc) noexcept;

    std::span<const std::uint8_t> buf_;
    std::size_t offset_ = 0;
    std::vector<std::uint8_t> record_;  // accumulator for the current logical record
};

}  // namespace merlion_tsdb::wal
