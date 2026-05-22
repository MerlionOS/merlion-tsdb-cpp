#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <system_error>

#include "merlion_tsdb/wal/page.hpp"

// File-backed WAL segment writer.
//
// A WAL is a directory of segment files named `00000000`, `00000001`, … No
// magic bytes; the segment index lives in the filename only. Records do not
// span segments — when the next record won't fit in the configured segment
// size (default 128 MiB), the current segment is closed (last page
// zero-padded, fsynced) and the writer rolls to the next index.
//
// See SPEC.md §4 and Go's `prometheus/tsdb/wlog/wlog.go`.
namespace merlion_tsdb::wal {

inline constexpr std::size_t k_default_segment_size = 128U * 1024U * 1024U;

class SegmentWriter {
public:
    // Opens a writer at `dir`. Creates the directory if it does not exist.
    // Scans existing `NNNNNNNN`-named files and continues at
    // `max(existing) + 1` (or 0 if none) so concurrent crash recovery works
    // out of the box.
    static std::expected<SegmentWriter, std::error_code>
    open(const std::filesystem::path& dir,
         std::size_t segment_size = k_default_segment_size);

    SegmentWriter(const SegmentWriter&) = delete;
    SegmentWriter& operator=(const SegmentWriter&) = delete;
    SegmentWriter(SegmentWriter&& other) noexcept;
    SegmentWriter& operator=(SegmentWriter&& other) noexcept;

    ~SegmentWriter();

    // Logs one logical record. Frames it, writes the new bytes to the
    // current segment file, and rolls over if needed. Does not fsync on
    // every call — see sync().
    [[nodiscard]] std::expected<void, std::error_code>
    log(std::span<const std::uint8_t> body);

    // fsyncs the current segment file. No-op if no segment is open.
    [[nodiscard]] std::expected<void, std::error_code> sync();

    // Closes the current segment cleanly: pads the last page, writes
    // remaining bytes, fsyncs, closes the fd. The next log() opens a
    // fresh segment at `current_segment_index() + 1`.
    [[nodiscard]] std::expected<void, std::error_code> cut();

    // Path of the segment file currently being written to.
    [[nodiscard]] std::filesystem::path current_segment_path() const;

    [[nodiscard]] std::size_t current_segment_index() const noexcept {
        return seg_index_;
    }

    [[nodiscard]] std::size_t segment_size() const noexcept { return segment_size_; }

    // Bytes already flushed to the kernel (i.e., the on-disk size of the
    // current segment). Does not include bytes still buffered in the
    // PageWriter waiting for the next log() to flush them.
    [[nodiscard]] std::size_t segment_bytes_on_disk() const noexcept {
        return flushed_bytes_;
    }

    // Releases the underlying segment file without flushing. Intended for
    // tests that want to simulate a crash mid-write. Returns the fd to the
    // caller; the SegmentWriter is left in a closed state.
    [[nodiscard]] int release_fd() noexcept;

private:
    SegmentWriter(std::filesystem::path dir, std::size_t segment_size,
                  std::size_t initial_index) noexcept;

    [[nodiscard]] std::expected<void, std::error_code> open_new_segment();
    [[nodiscard]] std::expected<void, std::error_code> flush_pending();
    [[nodiscard]] std::size_t framed_size_upper_bound(std::size_t body_len) const noexcept;

    std::filesystem::path dir_;
    std::size_t segment_size_;
    std::size_t seg_index_;
    int fd_ = -1;
    PageWriter pw_;
    // Bytes from pw_.bytes() already write()n to fd_. Less than or equal to
    // pw_.bytes().size(); the gap is the unflushed tail.
    std::size_t flushed_bytes_ = 0;
};

}  // namespace merlion_tsdb::wal
