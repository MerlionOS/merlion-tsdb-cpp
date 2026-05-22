#pragma once

#include <cstdint>
#include <string_view>

namespace merlion_tsdb::encoding {

// Mirrors the error categories used by Go's encoding layer.
// io.EOF        -> EndOfStream  (normal stream termination at a byte boundary)
// io.ErrUnexpectedEOF -> UnexpectedEnd  (ran out mid-record)
// overflow      -> VarintOverflow (uvarint exceeded 10 bytes)
enum class ReadError : std::uint8_t {
    EndOfStream,
    UnexpectedEnd,
    VarintOverflow,
};

constexpr std::string_view to_string(ReadError e) noexcept {
    switch (e) {
        case ReadError::EndOfStream:     return "end of stream";
        case ReadError::UnexpectedEnd:   return "unexpected end of stream";
        case ReadError::VarintOverflow:  return "varint overflow";
    }
    return "unknown";
}

}  // namespace merlion_tsdb::encoding
