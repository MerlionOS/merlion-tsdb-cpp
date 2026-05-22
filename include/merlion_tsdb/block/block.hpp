#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "merlion_tsdb/block/chunks.hpp"
#include "merlion_tsdb/block/index.hpp"
#include "merlion_tsdb/block/meta.hpp"
#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/model/labels.hpp"
#include "merlion_tsdb/model/matcher.hpp"

namespace merlion_tsdb::head { class Head; }

// Persistent-block reader — composes meta.json + index + chunks/ into one
// queryable handle.
//
// MVP surface (matches the consumer subset of upstream's `Block` —
// see prometheus/tsdb/block.go):
//
//   Block::open(dir)        // construct
//   block.meta()            // metadata
//   block.index()           // raw IndexReader for advanced use
//   block.chunks()          // raw ChunkReader for advanced use
//   block.query(name, val)  // convenience: find series + decoded chunks
//
// Out-of-scope for MVP: snapshots, deletions, tombstone application,
// concurrent-reader refcounting (upstream's Block.Close() blocks on
// pendingReaders). Tombstones are read but not consulted yet.
namespace merlion_tsdb::block {

class Block {
public:
    [[nodiscard]] static std::expected<Block, std::error_code>
    open(const std::filesystem::path& dir);

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&&) noexcept = default;
    Block& operator=(Block&&) noexcept = default;

    [[nodiscard]] const BlockMeta&    meta()   const noexcept { return meta_; }
    [[nodiscard]] const IndexReader&  index()  const noexcept { return index_; }
    [[nodiscard]] const ChunkReader&  chunks() const noexcept { return chunks_; }
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

    // One series's labels + every decoded chunk for that series. The
    // chunks are already CRC-checked and the XORChunk bytes are loaded;
    // call `.iterator()` to read samples.
    struct QueryResult {
        model::Labels                  labels;
        std::vector<chunkenc::XORChunk> chunks;
        std::vector<ChunkMeta>          chunk_metas;  // time ranges + refs
    };

    // Look up every series with the given (label name, label value) pair
    // and return its label set + decoded chunks. Returns an empty vector
    // (NOT an error) if no series matches.
    [[nodiscard]] std::expected<std::vector<QueryResult>, std::error_code>
    query(std::string_view name, std::string_view value) const;

    // §8 Querier surface. Matches every series whose labels satisfy ALL
    // matchers (logical AND) and intersects with the half-open time
    // range [mint, maxt]. Chunks that don't overlap the time range are
    // dropped; surviving chunks are decoded as XORChunks.
    //
    // `matchers` must contain at least one matcher — upstream Prometheus
    // forbids empty matcher sets to avoid accidentally selecting the
    // entire block. Returns an empty vector (NOT an error) if no series
    // satisfies the matchers.
    //
    // Time-range semantics match upstream: a chunk with [chunk.mint,
    // chunk.maxt] overlaps a query range [mint, maxt] when
    // chunk.mint <= maxt && chunk.maxt >= mint. Overlapping chunks are
    // returned in full — sample-level pruning is the caller's job.
    [[nodiscard]] std::expected<std::vector<QueryResult>, std::error_code>
    select(std::span<const model::Matcher> matchers,
           std::int64_t mint,
           std::int64_t maxt) const;

    // ---- Block creation API (head → block flush) -----------------------

    // One chunk worth of source data for a series being persisted to a
    // new block. The bytes are the full XORChunk byte buffer (including
    // the 2-byte sample-count header) — caller already produced these
    // from chunkenc::XORChunk::bytes() on a Head's MemSeries chunks.
    struct ChunkInput {
        std::int64_t              min_time = 0;
        std::int64_t              max_time = 0;
        std::vector<std::uint8_t> bytes;
    };

    // One series in the input set for a fresh block: a label set plus
    // its chunks in time order. An empty `chunks` vector skips the
    // series (matches upstream behaviour).
    struct SeriesInput {
        model::Labels           labels;
        std::vector<ChunkInput> chunks;
    };

    // Creates a new persistent block under `parent_dir`. Names the block
    // directory with a freshly-generated ULID; emits meta.json, chunks/,
    // and index. Builds the symbol table + postings internally.
    //
    // The atomic-create dance from upstream (tmp dir + rename) is
    // simplified: we write directly into <parent>/<ulid>/ since the
    // ULID is unique per call. Caller can layer their own
    // rename-on-success if they need strict atomicity across crashes.
    //
    // Returns the path of the created block directory.
    [[nodiscard]] static std::expected<std::filesystem::path, std::error_code>
    create_from_series(const std::filesystem::path& parent_dir,
                       std::span<const SeriesInput> series);

    // Thin adapter on top of create_from_series: walks every MemSeries
    // in `head`'s SeriesStore, extracts each chunk's bytes + computes
    // its (min_t, max_t) by iterating, and flushes a new block under
    // `parent_dir`. The Head itself isn't modified — this is a
    // read-only snapshot of in-memory state. Pending WAL records are
    // independent and unaffected.
    //
    // Forward-declared `head::Head` to avoid pulling head.hpp into this
    // header; the implementation lives in block.cpp.
    [[nodiscard]] static std::expected<std::filesystem::path, std::error_code>
    create_from_head(const std::filesystem::path& parent_dir,
                     const head::Head& head);

    // Compacts two or more existing blocks into a single new block under
    // `parent_dir`. For each series present in any input block, the
    // chunks are concatenated in `min_time`-ascending order. The new
    // block's `compaction.level` becomes `max(input levels) + 1`; its
    // `compaction.sources` is the union of every input's sources
    // (deduplicated, lex-sorted for determinism).
    //
    // Assumes input blocks have non-overlapping time ranges per series —
    // the standard compactor invariant. Overlap detection is not done
    // here; if two inputs cover the same (series, timestamp), both
    // chunks are kept and the output may contain duplicate samples on
    // query.
    //
    // The input directories are NOT deleted — the caller is responsible
    // for cleanup after verifying the new block.
    [[nodiscard]] static std::expected<std::filesystem::path, std::error_code>
    compact(const std::filesystem::path& parent_dir,
            std::span<const std::filesystem::path> input_block_dirs);

private:
    Block(std::filesystem::path dir,
          BlockMeta meta,
          IndexReader index,
          ChunkReader chunks) noexcept;

    std::filesystem::path dir_;
    BlockMeta             meta_;
    IndexReader           index_;
    ChunkReader           chunks_;
};

}  // namespace merlion_tsdb::block
