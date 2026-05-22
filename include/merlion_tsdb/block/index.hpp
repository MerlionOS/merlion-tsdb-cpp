#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// Top-level index reader. Phase 3a only exposes header / TOC / symbols;
// later phases add postings + series accessors.
class IndexReader {
public:
    [[nodiscard]] static std::expected<IndexReader, std::error_code>
    open(const std::filesystem::path& index_file);

    [[nodiscard]] std::uint8_t version() const noexcept { return version_; }
    [[nodiscard]] const IndexTOC& toc() const noexcept { return toc_; }
    [[nodiscard]] const IndexSymbolTable& symbols() const noexcept { return symbols_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {data_.data(), data_.size()};
    }

private:
    IndexReader(std::vector<std::uint8_t> data,
                std::uint8_t version,
                IndexTOC toc,
                IndexSymbolTable symbols) noexcept;

    std::vector<std::uint8_t> data_;
    std::uint8_t              version_ = 0;
    IndexTOC                  toc_;
    IndexSymbolTable          symbols_;
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

}  // namespace detail

}  // namespace merlion_tsdb::block
