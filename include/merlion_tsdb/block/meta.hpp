#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// On-disk block metadata (`meta.json`).
//
// One file per persistent block, sitting alongside `index`, `chunks/`, and
// `tombstones`. JSON wire format matches `tsdb/block.go`'s BlockMeta —
// upstream-produced files must round-trip through this module unchanged.
namespace merlion_tsdb::block {

inline constexpr int k_meta_version_1 = 1;
inline constexpr const char* k_meta_filename = "meta.json";

struct BlockStats {
    std::uint64_t num_samples           = 0;
    std::uint64_t num_float_samples     = 0;
    std::uint64_t num_histogram_samples = 0;
    std::uint64_t num_series            = 0;
    std::uint64_t num_chunks            = 0;
    std::uint64_t num_tombstones        = 0;

    friend bool operator==(const BlockStats&, const BlockStats&) = default;
};

// Short descriptor of a block referenced by another block's compaction
// history. Only the ULID + time range — no nested stats / compaction.
struct BlockDesc {
    std::string  ulid;
    std::int64_t min_time = 0;
    std::int64_t max_time = 0;

    friend bool operator==(const BlockDesc&, const BlockDesc&) = default;
};

struct BlockMetaCompaction {
    int                      level     = 0;
    std::vector<std::string> sources;       // ULIDs of source head blocks
    bool                     deletable = false;
    std::vector<BlockDesc>   parents;       // direct predecessor blocks
    bool                     failed    = false;
    std::vector<std::string> hints;         // sorted, see CompactionHint* in Go

    friend bool operator==(const BlockMetaCompaction&, const BlockMetaCompaction&) = default;
};

struct BlockMeta {
    int                 version   = k_meta_version_1;
    std::string         ulid;
    std::int64_t        min_time  = 0;
    std::int64_t        max_time  = 0;
    BlockStats          stats;
    BlockMetaCompaction compaction;

    friend bool operator==(const BlockMeta&, const BlockMeta&) = default;
};

// Reads `dir/meta.json`. Returns ec=invalid_argument if version is unknown.
[[nodiscard]] std::expected<BlockMeta, std::error_code>
read_meta(const std::filesystem::path& dir);

// Writes `dir/meta.json` atomically: serialises to `meta.json.tmp` first,
// then renames over the target so a crash mid-write can never leave a
// half-written meta file visible. fsyncs the directory after the rename
// on best-effort basis.
[[nodiscard]] std::expected<void, std::error_code>
write_meta(const std::filesystem::path& dir, const BlockMeta& meta);

// Convenience: serialises to a JSON string. Useful for tests / debugging.
[[nodiscard]] std::string encode_meta_json(const BlockMeta& meta);

// Convenience: parses a JSON string. Returns ec=invalid_argument on
// malformed JSON or unknown version.
[[nodiscard]] std::expected<BlockMeta, std::error_code>
decode_meta_json(std::string_view json_text);

}  // namespace merlion_tsdb::block
