#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "merlion_tsdb/chunkenc/encoding.hpp"
#include "merlion_tsdb/encoding/bstream.hpp"
#include "merlion_tsdb/encoding/error.hpp"

// XOR / Gorilla chunk encoding for float samples.
//
// See SPEC.md §3.1 for the byte-level wire format. Reference implementation:
// prometheus/tsdb/chunkenc/xor.go.
namespace merlion_tsdb::chunkenc {

class XORChunk;
class XORAppender;
class XORIterator;

class XORChunk {
public:
    // Two-byte big-endian sample-count header at the front of every chunk.
    static constexpr std::size_t   kHeaderSize = 2;
    static constexpr std::size_t   kMaxBytes   = 1024;
    static constexpr std::uint16_t kMaxSamples = std::numeric_limits<std::uint16_t>::max();

    XORChunk();

    // Wrap existing on-disk bytes. The caller is responsible for the bytes
    // being a well-formed XOR chunk — Iterator validates lazily.
    static XORChunk from_bytes(std::vector<std::uint8_t> bytes) {
        XORChunk c;
        c.writer_.reset(std::move(bytes));
        return c;
    }

    static constexpr Encoding encoding() noexcept { return Encoding::XOR; }

    [[nodiscard]] std::uint16_t num_samples() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return writer_.bytes();
    }

    // Returns an appender bound to this chunk. For a non-empty chunk this
    // replays the existing samples to recover the encoder state (t, v,
    // t_delta, leading, trailing). The appender holds a non-owning pointer
    // back to this chunk's BitWriter — the chunk must outlive the appender
    // and must not be moved while an appender is live.
    [[nodiscard]] std::expected<XORAppender, encoding::ReadError> appender();

    [[nodiscard]] XORIterator iterator() const noexcept;

    // Free the trailing capacity of the underlying buffer once the chunk
    // is known to be complete (matches Go's Compact semantics).
    void compact();

private:
    encoding::BitWriter writer_;
};

class XORAppender {
public:
    // Returns true if the sample was appended, false if the chunk is full
    // (num_samples already at u16::MAX). The Go implementation panics; we
    // return false so callers can cut a new chunk without an exception.
    bool append(std::int64_t t, double v) noexcept;

    [[nodiscard]] std::int64_t last_t()      const noexcept { return t_; }
    [[nodiscard]] double       last_value()  const noexcept { return v_; }

private:
    friend class XORChunk;

    explicit XORAppender(encoding::BitWriter* w) noexcept : writer_(w) {}

    XORAppender(encoding::BitWriter* w,
                std::int64_t t, double v, std::uint64_t t_delta,
                std::uint8_t leading, std::uint8_t trailing) noexcept
        : writer_(w), t_(t), v_(v), t_delta_(t_delta),
          leading_(leading), trailing_(trailing) {}

    void write_v_delta(double v) noexcept;

    encoding::BitWriter* writer_;
    std::int64_t  t_        = std::numeric_limits<std::int64_t>::min();
    double        v_        = 0.0;
    std::uint64_t t_delta_  = 0;
    // 0xFF is the "not initialized" sentinel from go-tsz. It forces the
    // first non-zero delta to emit a fresh (leading, trailing) window
    // rather than trying to reuse a meaningless previous one.
    std::uint8_t  leading_  = 0xFF;
    std::uint8_t  trailing_ = 0;
};

class XORIterator {
public:
    // Advances to the next sample. Returns true if a sample is available;
    // false at end-of-chunk or after an error. After a false return, check
    // error() to distinguish exhaustion from corruption.
    bool next() noexcept;

    [[nodiscard]] std::int64_t t() const noexcept { return t_; }
    [[nodiscard]] double       v() const noexcept { return v_; }
    [[nodiscard]] std::uint16_t num_total() const noexcept { return num_total_; }
    [[nodiscard]] std::uint16_t num_read()  const noexcept { return num_read_; }
    [[nodiscard]] std::optional<encoding::ReadError> error() const noexcept {
        return err_;
    }

private:
    friend class XORChunk;
    friend class XORAppender;

    explicit XORIterator(std::span<const std::uint8_t> bytes) noexcept;

    bool read_value() noexcept;

    encoding::BitReader reader_;
    std::uint16_t num_total_ = 0;
    std::uint16_t num_read_  = 0;
    std::int64_t  t_         = 0;
    double        v_         = 0.0;
    std::uint64_t t_delta_   = 0;
    std::uint8_t  leading_   = 0;
    std::uint8_t  trailing_  = 0;
    std::optional<encoding::ReadError> err_;
};

// XOR value encoding/decoding primitives. Exposed for future chunk encodings
// that share the algorithm (the histogram chunk reuses xor_write for the
// sum/count fields).
void xor_write(encoding::BitWriter& w, double new_v, double prev_v,
               std::uint8_t& leading, std::uint8_t& trailing) noexcept;

std::expected<void, encoding::ReadError>
xor_read(encoding::BitReader& r, double& value,
         std::uint8_t& leading, std::uint8_t& trailing) noexcept;

}  // namespace merlion_tsdb::chunkenc
