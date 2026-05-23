#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <system_error>
#include <vector>

#include "merlion_tsdb/block/block.hpp"
#include "merlion_tsdb/block/index.hpp"
#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/model/labels.hpp"
#include "merlion_tsdb/model/matcher.hpp"

// Forward declaration: §10 lets Querier mix in-memory Heads with
// persistent Blocks. We keep head/head.hpp out of this public header to
// avoid pulling its transitive WAL/segment dependencies into every
// querier consumer; the implementation in querier.cpp includes it.
namespace merlion_tsdb::head { class Head; }

// §9 cross-block + §10 head-aware querier. Holds N non-owning Block
// pointers plus optionally M non-owning Head pointers, runs a matcher
// set against each source, and merges results into a unified per-series
// view. The merge concatenates a series's chunks from every source that
// contained it and sorts the resulting chunk sequence by min_time
// ascending — matching upstream's chainedSeriesIterator invariants.
//
// Out of scope (deferred):
//   - Sample-level vertical merge across overlapping chunks. Cross-
//     source overlap is expected when a Head still holds samples that
//     have also been flushed to a fresh Block; chunk-level merge is
//     the right level for MVP, and §7 already does sample-level dedup
//     inside compaction. Sample-level dedup at query time is a future
//     enhancement (cf. upstream's `storage.NewMergeSeriesSet`).
//   - sortSeries / SelectHints from upstream. Results are returned in
//     hash-bucket order; deterministic ordering is the caller's job.
namespace merlion_tsdb::querier {

// One series's labels plus every decoded chunk that survived matchers
// + time range, from every contributing block. Chunks are sorted by
// `chunk_metas[i].min_time` ascending. The two vectors are parallel:
// `chunks[i]` corresponds to `chunk_metas[i]`.
struct MergedSeries {
    model::Labels                            labels;
    std::vector<chunkenc::XORChunk>          chunks;
    std::vector<block::ChunkMeta>            chunk_metas;
};

class Querier {
public:
    // Constructs a Querier over a fixed set of blocks. The Block
    // pointers are non-owning and must outlive the Querier.
    explicit Querier(std::span<const block::Block* const> blocks);

    // §10 ctor: blocks + heads. Either span may be empty. Pointers in
    // both spans are non-owning and must outlive the Querier.
    Querier(std::span<const block::Block* const> blocks,
            std::span<const head::Head* const>   heads);

    Querier(const Querier&) = delete;
    Querier& operator=(const Querier&) = delete;
    Querier(Querier&&) = default;
    Querier& operator=(Querier&&) = default;

    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }
    [[nodiscard]] std::size_t head_count()  const noexcept { return heads_.size(); }

    // Run `matchers` (logical AND) against every block AND every head,
    // restricted to `[mint, maxt]`. For each label set present in any
    // source, returns a MergedSeries with chunks from every
    // contributing source sorted by min_time. A block whose meta time
    // range is fully outside the query range is skipped entirely (no
    // I/O); per-chunk filtering inside Head::select / Block::select
    // handles the remaining sources.
    //
    // Empty `matchers` is rejected (matches Block::select).
    // Returns an empty vector if no series matches.
    [[nodiscard]] std::expected<std::vector<MergedSeries>, std::error_code>
    select(std::span<const model::Matcher> matchers,
           std::int64_t mint,
           std::int64_t maxt) const;

private:
    std::vector<const block::Block*> blocks_;
    std::vector<const head::Head*>   heads_;
};

}  // namespace merlion_tsdb::querier
