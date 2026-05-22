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

// §9 cross-block querier. Holds N non-owning Block pointers, runs a
// matcher set against each, and merges results into a unified
// per-series view. The merge concatenates a series's chunks from every
// block that contained it and sorts the resulting chunk sequence by
// min_time ascending — matching upstream's chainedSeriesIterator
// invariants.
//
// Out of scope (deferred):
//   - Sample-level vertical merge across overlapping chunks. Cross-
//     block overlap is rare at the same compaction level (blocks are
//     time-disjoint by construction) but can occur during compaction
//     windows; chunk-level merge is the right level for MVP, and
//     §7 already does sample-level dedup inside a single block.
//   - Head support. Head queries are deferred to a separate adapter
//     (planned: §10).
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

    Querier(const Querier&) = delete;
    Querier& operator=(const Querier&) = delete;
    Querier(Querier&&) = default;
    Querier& operator=(Querier&&) = default;

    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }

    // Run `matchers` (logical AND) against every block, restricted to
    // `[mint, maxt]`. For each label set present in any block, returns
    // a MergedSeries with chunks from every contributing block sorted
    // by min_time. A block whose meta time range is fully outside the
    // query range is skipped entirely (no I/O); per-chunk filtering
    // mirrors Block::select for the remaining blocks.
    //
    // Empty `matchers` is rejected (matches Block::select).
    // Returns an empty vector if no series matches.
    [[nodiscard]] std::expected<std::vector<MergedSeries>, std::error_code>
    select(std::span<const model::Matcher> matchers,
           std::int64_t mint,
           std::int64_t maxt) const;

private:
    std::vector<const block::Block*> blocks_;
};

}  // namespace merlion_tsdb::querier
