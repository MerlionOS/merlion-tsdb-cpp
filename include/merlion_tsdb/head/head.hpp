#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

#include "merlion_tsdb/block/index.hpp"        // ChunkMeta — reused as the chunk handoff type
#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/head/series_store.hpp"
#include "merlion_tsdb/model/labels.hpp"
#include "merlion_tsdb/model/matcher.hpp"
#include "merlion_tsdb/wal/record.hpp"
#include "merlion_tsdb/wal/segment_writer.hpp"

// Head block: in-memory time-series state backed by a WAL on disk.
//
// `Head::open(dir)` creates (or opens) a directory layout:
//
//   dir/
//     wal/
//       00000000
//       00000001
//       ...
//
// Phase 3 (this file) covers the write path: append samples into
// MemSeries chunks and queue Series + SamplesV2 records for the WAL.
// Phase 4 adds replay so reopening a directory rebuilds the in-memory
// state from the WAL.
namespace merlion_tsdb::head {

class Head {
public:
    // Opens (or creates) a head block rooted at `dir`. Creates the
    // directory and its `wal/` subdirectory if needed. If existing
    // segments are found under `dir/wal/`, they are replayed first to
    // reconstruct the in-memory series + sample state, then the segment
    // writer opens at the next free index for new writes. A torn record
    // at the tail of the last segment is dropped silently — same
    // behaviour as Go's WAL replay.
    [[nodiscard]] static std::expected<Head, std::error_code>
    open(const std::filesystem::path& dir);

    Head(const Head&) = delete;
    Head& operator=(const Head&) = delete;
    Head(Head&& other) noexcept;
    Head& operator=(Head&& other) noexcept;
    ~Head();

    // Appends one sample to the series identified by `labels`. Creates
    // the series on demand. Returns the series ref. Pending series and
    // samples are buffered until commit() — durability is only
    // guaranteed after commit() returns success.
    [[nodiscard]] std::expected<SeriesRef, std::error_code>
    append(const model::Labels& labels, std::int64_t t, double v);

    // Flushes pending records to the WAL. Emits at most one Series
    // record (containing all newly-created series since last commit)
    // and at most one SamplesV2 record (containing all buffered
    // samples). fsyncs the WAL segment so the records are durable
    // after this call returns success.
    [[nodiscard]] std::expected<void, std::error_code> commit();

    // Final commit + segment cut + writer close. The destructor calls
    // close() implicitly; the explicit form lets callers see errors.
    // After close, every method except destruction returns an error.
    [[nodiscard]] std::expected<void, std::error_code> close();

    [[nodiscard]] const SeriesStore& series() const noexcept { return series_; }
    [[nodiscard]] SeriesStore&       series()       noexcept { return series_; }
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
    [[nodiscard]] std::filesystem::path wal_dir() const { return dir_ / "wal"; }

    // §10 Querier surface. Mirrors Block::select: returns each in-memory
    // series whose labels satisfy ALL `matchers` (logical AND) intersected
    // with the inclusive time range [mint, maxt]. Chunks that don't
    // overlap the range are dropped; the surviving chunks' byte buffers
    // are **copied** into freshly-wrapped XORChunks so the result outlives
    // any subsequent append to the underlying MemSeries.
    //
    // `block::ChunkMeta` is reused as the chunk handoff type; `ref` is
    // left as 0 for in-memory chunks (the chunk-ref namespace only makes
    // sense for on-disk chunks). `min_time` / `max_time` are recovered by
    // a one-pass iteration of each surviving chunk — XOR encoding emits
    // samples in monotonic-t order so first sample is min, last is max.
    //
    // Empty matcher sets are rejected (same rule as Block::select).
    // A non-empty result with an empty `chunks` vector is impossible —
    // series with no surviving chunks are dropped entirely.
    //
    // MVP is single-threaded: callers must not append() concurrently with
    // select(). Stripe locking + snapshot stability are future work.
    struct QueryResult {
        model::Labels                     labels;
        std::vector<chunkenc::XORChunk>   chunks;
        std::vector<block::ChunkMeta>     chunk_metas;
    };

    [[nodiscard]] std::expected<std::vector<QueryResult>, std::error_code>
    select(std::span<const model::Matcher> matchers,
           std::int64_t mint,
           std::int64_t maxt) const;

    [[nodiscard]] std::size_t pending_series()  const noexcept {
        return pending_series_.size();
    }
    [[nodiscard]] std::size_t pending_samples() const noexcept {
        return pending_samples_.size();
    }

private:
    Head(std::filesystem::path dir, wal::SegmentWriter wal) noexcept;

    std::filesystem::path                dir_;
    wal::SegmentWriter                   wal_;
    SeriesStore                          series_;
    std::vector<wal::record::RefSeries>  pending_series_;
    std::vector<wal::record::RefSample>  pending_samples_;
    bool                                 closed_ = false;
};

}  // namespace merlion_tsdb::head
