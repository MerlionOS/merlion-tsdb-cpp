#pragma once

#include <cstdint>
#include <string_view>

namespace merlion_tsdb::chunkenc {

// Tag byte stored on disk to identify the chunk encoding. Values must match
// Go's tsdb/chunkenc/chunk.go Encoding iota.
enum class Encoding : std::uint8_t {
    None           = 0,
    XOR            = 1,
    Histogram      = 2,
    FloatHistogram = 3,
    XOR2           = 4,
};

constexpr std::string_view to_string(Encoding e) noexcept {
    switch (e) {
        case Encoding::None:           return "none";
        case Encoding::XOR:            return "XOR";
        case Encoding::Histogram:      return "histogram";
        case Encoding::FloatHistogram: return "floathistogram";
        case Encoding::XOR2:           return "XOR2";
    }
    return "<unknown>";
}

// Logical kind of value an iterator currently exposes.
enum class ValueType : std::uint8_t {
    None           = 0,
    Float          = 1,
    Histogram      = 2,
    FloatHistogram = 3,
};

}  // namespace merlion_tsdb::chunkenc
