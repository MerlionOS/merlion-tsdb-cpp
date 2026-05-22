#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "merlion_tsdb/wal/page.hpp"

// Directory-aware WAL segment reader. Iterates records across all segment
// files in lexicographic (== numeric) order. The last segment may be
// truncated mid-record if the writer crashed; that's not an error — the
// torn record is dropped and the iterator reports clean EndOfStream.
//
// Records in non-last segments must NOT be torn (that's data corruption,
// since records do not span segments).
namespace merlion_tsdb::wal {

// Distinguishes "no more records" from real errors. Mirrors the WalReadError
// granularity but adds DirectoryRead for I/O failures opening segments.
enum class SegmentReadError : std::uint8_t {
    EndOfStream      = 0,
    DirectoryRead    = 1,
    SegmentOpen      = 2,
    CorruptMidStream = 3,  // torn/CRC/etc. in a non-last segment
    Crc              = 4,
    InvalidFraming   = 5,
    UnsupportedCompression = 6,
};

class SegmentReader {
public:
    // Opens `dir` for replay. Scans for numbered segment files and orders
    // them by index. Returns DirectoryRead on filesystem failure.
    static std::expected<SegmentReader, std::error_code>
    open(const std::filesystem::path& dir);

    // Returns the next logical record, or EndOfStream when all segments
    // are cleanly exhausted. A torn record at the tail of the LAST segment
    // is treated as clean EOF; a torn record anywhere else is reported as
    // CorruptMidStream.
    std::expected<std::span<const std::uint8_t>, SegmentReadError> next();

    // Total number of segments discovered at open() time.
    [[nodiscard]] std::size_t segment_count() const noexcept {
        return segment_paths_.size();
    }

    // Path of the segment currently being read (or empty if exhausted).
    [[nodiscard]] std::filesystem::path current_segment_path() const;

    // Index of the segment currently being read.
    [[nodiscard]] std::size_t current_segment_index() const noexcept {
        return current_index_ < segment_paths_.size() ? current_index_ : 0;
    }

private:
    explicit SegmentReader(std::vector<std::filesystem::path> segments) noexcept
        : segment_paths_(std::move(segments)) {}

    [[nodiscard]] std::expected<void, SegmentReadError> load_current_segment();

    std::vector<std::filesystem::path> segment_paths_;
    std::size_t current_index_ = 0;
    std::vector<std::uint8_t> current_bytes_;
    // Lazily constructed once current_bytes_ is loaded. Wrapped in optional
    // to delay construction until we know we have a segment to read.
    std::optional<PageReader> page_reader_;
};

}  // namespace merlion_tsdb::wal
