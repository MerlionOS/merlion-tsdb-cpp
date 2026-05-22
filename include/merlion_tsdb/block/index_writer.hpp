#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "merlion_tsdb/block/index.hpp"

// Persistent-block index file writer (v2/v3 wire layout — upstream
// actually writes V2; V3 readers accept the same bytes, just use a
// sparser in-memory offset table).
//
// Buffered single-pass writer: holds the entire index in memory, emits
// it on `close()`. Suitable for any block size we'd flush from a head
// (typical blocks are under 100 MiB of index bytes); upstream's temp-
// file dance exists because Go can't afford to hold everything in
// memory for very large compactions — not a concern for the MVP.
//
// Phase order is enforced:
//   open() -> add_symbol()* -> finish_symbols()
//          -> add_series()* -> finish_series()
//          -> add_postings()* -> close()
//
// Reference: prometheus/tsdb/index/index.go Writer (line 128+),
// AddSymbol (line 540), AddSeries (line 463), writePosting (line 846),
// writeTOC (line 691).
namespace merlion_tsdb::block {

inline constexpr int          k_index_write_format    = 2;  // V2 — what upstream emits
inline constexpr std::size_t  k_index_series_align    = 16; // each series rounded up

class IndexWriter {
public:
    // Creates a new index file. Refuses to overwrite an existing file.
    [[nodiscard]] static std::expected<IndexWriter, std::error_code>
    create(const std::filesystem::path& file);

    IndexWriter(const IndexWriter&) = delete;
    IndexWriter& operator=(const IndexWriter&) = delete;
    IndexWriter(IndexWriter&&) noexcept = default;
    IndexWriter& operator=(IndexWriter&&) noexcept = default;

    // Adds one symbol. Must be called in strictly ascending lex order
    // (upstream enforces this; emitting out-of-order would break the
    // sparse-offset reader). Returns the symbol's u32 ref for use by
    // add_series.
    [[nodiscard]] std::expected<std::uint32_t, std::error_code>
    add_symbol(std::string_view sym);

    // Closes the symbol section: backfills its length + CRC32C and
    // records the offset for the TOC. Must be called exactly once
    // after the last add_symbol and before the first add_series.
    [[nodiscard]] std::expected<void, std::error_code> finish_symbols();

    // Adds one series. The labels are pairs of (name_ref, value_ref)
    // into the symbol table, in canonical (sorted-by-name, deduped)
    // order. The chunk_metas list carries time ranges + BlockChunkRef
    // packed u64s. Returns the series' u32 ID (= byte_offset / 16),
    // which the caller should record for posting-list emission.
    [[nodiscard]] std::expected<std::uint32_t, std::error_code>
    add_series(std::span<const std::pair<std::uint32_t, std::uint32_t>> label_refs,
               std::span<const ChunkMeta> chunk_metas);

    // Closes the series section: records the postings section start in
    // the TOC. After this, no more series may be added.
    [[nodiscard]] std::expected<void, std::error_code> finish_series();

    // Adds one posting list (label name=value → sorted series IDs).
    // Series IDs MUST be in strictly ascending order; the on-disk
    // posting list format relies on that for iteration efficiency.
    [[nodiscard]] std::expected<void, std::error_code>
    add_postings(std::string_view name,
                 std::string_view value,
                 std::span<const std::uint32_t> series_ids);

    // Finalizes: emits the postings offset table + TOC, writes the
    // whole buffer to disk, fsyncs, closes the underlying fd.
    [[nodiscard]] std::expected<void, std::error_code> close();

    [[nodiscard]] std::size_t bytes_so_far() const noexcept { return buf_.size(); }

private:
    enum class Stage { Header, Symbols, Series, Postings, Done };

    explicit IndexWriter(std::filesystem::path file) noexcept
        : file_(std::move(file)) {}

    // Buffer helpers.
    void put_u8(std::uint8_t v);
    void put_be32(std::uint32_t v);
    void put_be64(std::uint64_t v);
    void put_uvarint(std::uint64_t v);
    void put_varint(std::int64_t v);
    void put_uvarint_str(std::string_view s);

    // Patch a previously-reserved BE u32 (used for length backfill).
    void patch_be32(std::size_t offset, std::uint32_t v);

    // Append CRC32C(buf_[start..end)) as BE u32. Used to terminate sections.
    void append_crc(std::size_t start, std::size_t end);

    // Pad buf_ with zero bytes up to the next multiple of `align`.
    void pad_to(std::size_t align);

    std::filesystem::path           file_;
    std::vector<std::uint8_t>       buf_;
    Stage                           stage_           = Stage::Header;
    std::uint32_t                   next_sym_ref_    = 0;
    std::string                     last_symbol_;
    bool                            any_symbol_      = false;
    // Backfill point for the symbol section's length field.
    std::size_t                     symbols_len_off_ = 0;
    // TOC offsets — filled in across phase transitions.
    std::uint64_t                   off_symbols_     = 0;
    std::uint64_t                   off_series_      = 0;
    std::uint64_t                   off_label_idx_   = 0;
    std::uint64_t                   off_label_tbl_   = 0;
    std::uint64_t                   off_postings_    = 0;
    std::uint64_t                   off_postings_tbl_ = 0;
    // Series count + per-series start offsets (debug aid; not written to disk).
    std::uint32_t                   series_count_    = 0;
    // Pending postings offset-table entries: collected until close().
    struct PendingEntry {
        std::string  name;
        std::string  value;
        std::uint64_t offset;  // absolute offset of the posting list's length prefix
    };
    std::vector<PendingEntry>       pending_postings_;
};

}  // namespace merlion_tsdb::block
