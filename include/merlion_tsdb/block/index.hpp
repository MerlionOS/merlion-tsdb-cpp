#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "merlion_tsdb/block/chunks.hpp"
#include "merlion_tsdb/model/labels.hpp"

// Persistent-block index file reader.
//
// Index file layout:
//
//   header           : magic (4 BE = 0xBAAAD700) | version (1 byte)
//   <variable-size sections — symbols, series, label indices, postings,
//                              postings offset table — in any order>
//   TOC              : 6 u64 BE section offsets + u32 BE CRC32C  (last 52 bytes)
//
// Every section is framed as:
//   u32 BE length
//   `length` bytes of payload
//   u32 BE CRC32C of the payload (NOT including the length prefix)
//
// This PR (block phase 3a) covers the header + TOC + symbol table. The
// remaining sections (series, postings, postings offset table) follow in
// phase 3b and 3c.
//
// Version handling:
//   V1: symbol refs are absolute byte offsets into the index file.
//   V3: symbol refs are 0-based integer indices; lookup uses a sparse
//       (every-32nd-symbol) offset table.
//   V2 has the same symbol semantics as V3 but is only present in
//   transitional fixtures. The reader treats V2 like V3 for symbol
//   lookups.
//
// The upstream golden block at testdata/index_format_v1/ is V1; that's
// what we validate against here.
//
// Reference: prometheus/tsdb/index/index.go (constants line 39-58,
// TOC line 170-203, NewSymbols line 1167-1191, Lookup line 1193-1215).
namespace merlion_tsdb::block {

inline constexpr std::uint32_t k_magic_index       = 0xBAAAD700U;
inline constexpr std::uint8_t  k_index_format_v1   = 1;
inline constexpr std::uint8_t  k_index_format_v2   = 2;
inline constexpr std::uint8_t  k_index_format_v3   = 3;
inline constexpr std::size_t   k_index_header_size = 5;   // 4 magic + 1 version
inline constexpr std::size_t   k_index_toc_size    = 6U * 8U + 4U;  // 6 u64 offsets + CRC

// Table of contents at the end of the file. All offsets are absolute byte
// positions in the file.
struct IndexTOC {
    std::uint64_t symbols             = 0;
    std::uint64_t series              = 0;
    std::uint64_t label_indices       = 0;
    std::uint64_t label_indices_table = 0;
    std::uint64_t postings            = 0;
    std::uint64_t postings_table      = 0;

    friend bool operator==(const IndexTOC&, const IndexTOC&) = default;
};

// Symbol-table view over an index file's symbols section.
class IndexSymbolTable {
public:
    // Reads count and (for V3) builds the sparse offset table. `payload`
    // must point at the symbol section's payload bytes (after the 4-byte
    // length prefix; CRC already validated).
    [[nodiscard]] static std::expected<IndexSymbolTable, std::error_code>
    parse(std::span<const std::uint8_t> file_bytes,
          std::size_t section_offset,
          std::uint8_t version);

    [[nodiscard]] std::uint32_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint8_t  version() const noexcept { return version_; }

    // Look up a symbol by ref. V1 refs are absolute file offsets; V2/V3
    // refs are 0..count_-1 indices.
    [[nodiscard]] std::expected<std::string_view, std::error_code>
    lookup(std::uint32_t ref) const;

    // Iterate every symbol in stored order. Useful for sanity checks and
    // for the reverse-lookup helper that the series decoder will need.
    [[nodiscard]] std::vector<std::string> all_symbols() const;

private:
    IndexSymbolTable(std::span<const std::uint8_t> file_bytes,
                     std::uint8_t version,
                     std::uint32_t count,
                     std::size_t payload_offset,
                     std::vector<std::size_t> sparse_offsets) noexcept;

    std::span<const std::uint8_t> file_bytes_;   // borrowed; outlives this object
    std::uint8_t                  version_         = 0;
    std::uint32_t                 count_           = 0;
    std::size_t                   payload_offset_  = 0;  // absolute file offset of count field
    std::vector<std::size_t>      sparse_offsets_;       // V3 only: every 32nd entry
};

// Single entry in the postings offset table: a (label name, label value)
// → posting list location pair. The offset is an absolute byte position in
// the index file, pointing at the posting list's length prefix.
struct PostingsOffsetEntry {
    std::string   name;
    std::string   value;
    std::uint64_t offset = 0;

    friend bool operator==(const PostingsOffsetEntry&, const PostingsOffsetEntry&) = default;
};

// Read-only view of the postings offset table (TOC section
// `postings_table`). Holds the parsed entries plus a (name, value) ->
// offset lookup map.
class PostingsOffsetTable {
public:
    [[nodiscard]] static std::expected<PostingsOffsetTable, std::error_code>
    parse(std::span<const std::uint8_t> file_bytes, std::size_t section_offset);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::span<const PostingsOffsetEntry> entries() const noexcept {
        return {entries_.data(), entries_.size()};
    }

    // Returns the posting-list offset for (name, value), or empty if no
    // such label pair has a posting list. Upstream does NOT guarantee the
    // entries are sorted by (name, value), so we use a hash map built at
    // parse time.
    [[nodiscard]] std::optional<std::uint64_t>
    lookup(std::string_view name, std::string_view value) const;

private:
    explicit PostingsOffsetTable(std::vector<PostingsOffsetEntry> entries);

    std::vector<PostingsOffsetEntry> entries_;
    // (name + '\0' + value) -> offset. Owning string keys; the entries_
    // vector survives moves so it's also safe to point string_views at
    // it, but a small owning map is simpler and dwarfed by entries_.
    std::unordered_map<std::string, std::uint64_t> by_pair_;
};

// Decodes one posting list at the given offset and returns its sorted
// series refs. `offset` points at the list's 4-byte length prefix
// (matches the value stored in the postings offset table).
[[nodiscard]] std::expected<std::vector<std::uint32_t>, std::error_code>
read_posting_list(std::span<const std::uint8_t> file_bytes, std::uint64_t offset);

// Per-chunk metadata stored on each series entry. `ref` is the
// BlockChunkRef u64 packing (file_seq << 32 | byte_offset) pointing into
// the block's chunks/ directory.
struct ChunkMeta {
    std::int64_t  min_time = 0;
    std::int64_t  max_time = 0;
    std::uint64_t ref      = 0;

    [[nodiscard]] BlockChunkRef chunk_ref() const noexcept {
        return BlockChunkRef::from_u64(ref);
    }
    friend bool operator==(const ChunkMeta&, const ChunkMeta&) = default;
};

// One decoded series — label set (sorted, canonical) + chunk meta list.
struct SeriesEntry {
    model::Labels          labels;
    std::vector<ChunkMeta> chunks;
    friend bool operator==(const SeriesEntry&, const SeriesEntry&) = default;
};

// Top-level index reader. Phase 3a exposes header / TOC / symbols; 3b
// adds the postings offset table + posting list access.
class IndexReader {
public:
    [[nodiscard]] static std::expected<IndexReader, std::error_code>
    open(const std::filesystem::path& index_file);

    [[nodiscard]] std::uint8_t version() const noexcept { return version_; }
    [[nodiscard]] const IndexTOC& toc() const noexcept { return toc_; }
    [[nodiscard]] const IndexSymbolTable& symbols() const noexcept { return symbols_; }
    [[nodiscard]] const PostingsOffsetTable& postings_table() const noexcept {
        return postings_table_;
    }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {data_.data(), data_.size()};
    }

    // Convenience: look up a (name, value) and return the posting list's
    // sorted series refs in one call. Returns empty if no such pair.
    [[nodiscard]] std::expected<std::vector<std::uint32_t>, std::error_code>
    postings(std::string_view name, std::string_view value) const;

    // Sorted, deduped union of every posting list whose label name is
    // `name`. Equivalent to `Σ postings(name, v) for each v under name`.
    // Returns empty if `name` has no posting lists at all (i.e. the
    // label is absent from every series).
    [[nodiscard]] std::expected<std::vector<std::uint32_t>, std::error_code>
    postings_for_name(std::string_view name) const;

    // Sorted, deduped union of every posting list in the block — every
    // series id that appears anywhere. Used by Neq/Nre/regex-matches-
    // empty matchers to compute the complement. Built by walking the
    // postings offset table once.
    [[nodiscard]] std::expected<std::vector<std::uint32_t>, std::error_code>
    all_postings() const;

    // Every distinct label value with a non-empty posting list under
    // `name`. Lexicographically sorted. Used by Re/Nre matchers to
    // expand a regex into the matching subset of posting lists.
    [[nodiscard]] std::vector<std::string>
    label_values(std::string_view name) const;

    // Decode one series entry by its ID (as written in posting lists).
    // V1: id is the absolute file offset of the entry's length prefix.
    // V2/V3: id is the byte offset divided by 16 (series entries are
    //        16-byte aligned).
    [[nodiscard]] std::expected<SeriesEntry, std::error_code>
    series(std::uint64_t id) const;

private:
    IndexReader(std::vector<std::uint8_t> data,
                std::uint8_t version,
                IndexTOC toc,
                IndexSymbolTable symbols,
                PostingsOffsetTable postings_table) noexcept;

    std::vector<std::uint8_t> data_;
    std::uint8_t              version_ = 0;
    IndexTOC                  toc_;
    IndexSymbolTable          symbols_;
    PostingsOffsetTable       postings_table_;
};

// Helpers exposed for unit tests and the later index-section readers.
namespace detail {

[[nodiscard]] std::expected<IndexTOC, std::error_code>
parse_toc(std::span<const std::uint8_t> file_bytes);

// Validates and returns a span over a section's payload bytes. `offset`
// is the absolute file offset of the section's length prefix. Returns
// the payload span (length prefix and CRC consumed + validated).
[[nodiscard]] std::expected<std::span<const std::uint8_t>, std::error_code>
read_section_payload(std::span<const std::uint8_t> file_bytes,
                     std::size_t offset);

// Variant of read_section_payload where the length prefix is a uvarint
// instead of a u32 BE. Matches Go's NewDecbufUvarintAt and is used by
// the series section.
[[nodiscard]] std::expected<std::span<const std::uint8_t>, std::error_code>
read_uvarint_section_payload(std::span<const std::uint8_t> file_bytes,
                             std::size_t offset);

// Decode a single series entry from a raw payload span (the inner bytes
// already unwrapped from the uvarint-length + CRC framing). Exposed so
// tests and the future block writer can exercise the decoder directly.
[[nodiscard]] std::expected<SeriesEntry, std::error_code>
decode_series_payload(std::span<const std::uint8_t> payload,
                      const IndexSymbolTable& symbols);

}  // namespace detail

}  // namespace merlion_tsdb::block
