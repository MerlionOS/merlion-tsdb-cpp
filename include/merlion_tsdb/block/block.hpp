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
