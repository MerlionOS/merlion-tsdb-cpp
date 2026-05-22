#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// WAL record encoders and decoders for the subset needed by an MVP head
// replay: Series (type 1) and SamplesV2 (type 11). Older Samples V1
// (type 2) and the histogram / exemplar / metadata variants are deferred
// — they share the same encoding primitives so adding them later is mostly
// boilerplate.
//
// Reference: prometheus/tsdb/record/record.go (Encoder::Series, samplesV2
// and their counterparts on Decoder).
namespace merlion_tsdb::wal::record {

// Type byte that prefixes every record body. Values match Go's iota.
enum class Type : std::uint8_t {
    Unknown                            = 255,
    Series                             = 1,
    Samples                            = 2,
    Tombstones                         = 3,
    Exemplars                          = 4,
    MmapMarkers                        = 5,
    Metadata                           = 6,
    HistogramSamples                   = 7,
    FloatHistogramSamples              = 8,
    CustomBucketsHistogramSamples      = 9,
    CustomBucketsFloatHistogramSamples = 10,
    SamplesV2                          = 11,
    HistogramSamplesV2                 = 12,
    FloatHistogramSamplesV2            = 13,
};

// Returns the leading type byte of `rec`, or Unknown if the record is empty.
constexpr Type peek_type(std::span<const std::uint8_t> rec) noexcept {
    if (rec.empty()) return Type::Unknown;
    return static_cast<Type>(rec.front());
}

// SeriesRef is a uint64 in upstream. We use the same width; signedness only
// matters for the V2 sample-ref delta, which Prometheus computes via int64
// subtraction (so a delta can be negative when series refs decrease).
using SeriesRef = std::uint64_t;

struct Label {
    std::string name;
    std::string value;

    friend bool operator==(const Label&, const Label&) = default;
};

// Series record entry.
struct RefSeries {
    SeriesRef ref = 0;
    std::vector<Label> labels;

    friend bool operator==(const RefSeries&, const RefSeries&) = default;
};

// Sample record entry. ST (start timestamp) is 0 when absent; for V2 it
// encodes via a marker byte.
struct RefSample {
    SeriesRef    ref = 0;
    std::int64_t t   = 0;
    std::int64_t st  = 0;
    double       v   = 0.0;

    friend bool operator==(const RefSample& a, const RefSample& b) noexcept {
        // bit_cast comparison so NaN values roundtrip cleanly.
        return a.ref == b.ref && a.t == b.t && a.st == b.st &&
               std::bit_cast<std::uint64_t>(a.v) ==
                   std::bit_cast<std::uint64_t>(b.v);
    }
};

// Decode errors.
enum class RecordError : std::uint8_t {
    InvalidType    = 0,  // type byte didn't match the expected record kind
    UnexpectedEnd  = 1,  // ran out of bytes mid-field
    TrailingBytes  = 2,  // record body had extra bytes after the last entry
    LabelTooLong   = 3,  // uvarint length overflowed the buffer
    StMarkerInvalid = 4,  // ST marker byte was not in {0, 1, 2}
};

// --- Series (type = 1) ----------------------------------------------------

std::vector<std::uint8_t> encode_series(std::span<const RefSeries> series);

std::expected<std::vector<RefSeries>, RecordError>
decode_series(std::span<const std::uint8_t> rec);

// --- SamplesV2 (type = 11) ------------------------------------------------

std::vector<std::uint8_t> encode_samples_v2(std::span<const RefSample> samples);

std::expected<std::vector<RefSample>, RecordError>
decode_samples_v2(std::span<const std::uint8_t> rec);

}  // namespace merlion_tsdb::wal::record
