#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/model/labels.hpp"

// In-memory state for a single time series in the head block.
//
// Holds an ordered list of XOR chunks, the active appender, and the last
// observed timestamp (for monotonic-append enforcement). Mirrors the
// essential shape of `memSeries` in prometheus/tsdb/head.go:2508 minus the
// concurrency machinery — single-writer in the MVP.
namespace merlion_tsdb::head {

using SeriesRef = std::uint64_t;

// Pre-cut threshold from upstream: cut a new chunk when the current one is
// within 19 bytes of the 1 KiB cap, because a single sample can grow the
// chunk by up to that much (see chunkenc/chunk.go:62, MaxBytesPerXORChunk).
inline constexpr std::size_t k_max_bytes_per_chunk_before_append =
    chunkenc::XORChunk::kMaxBytes - 19U;

class MemSeries {
public:
    MemSeries(SeriesRef ref, model::Labels labels);

    [[nodiscard]] SeriesRef            ref()    const noexcept { return ref_; }
    [[nodiscard]] const model::Labels& labels() const noexcept { return labels_; }

    // Appends a (t, v) sample. Returns true on success; false if `t` is
    // older than the last observed timestamp (timestamps must be
    // monotonically non-decreasing within a series).
    bool append(std::int64_t t, double v);

    [[nodiscard]] std::int64_t last_t()      const noexcept { return last_t_; }
    [[nodiscard]] double       last_value()  const noexcept { return last_v_; }
    [[nodiscard]] std::size_t  num_chunks()  const noexcept { return chunks_.size(); }
    [[nodiscard]] std::uint64_t num_samples() const noexcept { return num_samples_; }

    // Iterator-friendly read-only view of all chunks in this series, in
    // append order (oldest first, current chunk last). Pointers remain
    // valid until the next append() that triggers a chunk cut.
    [[nodiscard]] std::vector<const chunkenc::XORChunk*> chunks() const;

    // Drops all samples and chunks. Used by tests; not used in steady state.
    void reset();

private:
    void cut_chunk();

    SeriesRef    ref_;
    model::Labels labels_;
    std::vector<std::unique_ptr<chunkenc::XORChunk>> chunks_;
    // Appender into chunks_.back(). Cleared when cutting; rebuilt on next
    // append. Wrapped in optional so we can default-construct the series
    // with no chunks then create one lazily.
    std::optional<chunkenc::XORAppender> current_appender_;
    std::int64_t last_t_      = std::numeric_limits<std::int64_t>::min();
    double       last_v_      = 0.0;
    std::uint64_t num_samples_ = 0;
    bool         has_sample_  = false;
};

}  // namespace merlion_tsdb::head
