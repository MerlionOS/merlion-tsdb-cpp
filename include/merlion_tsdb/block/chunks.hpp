#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

#include "merlion_tsdb/chunkenc/encoding.hpp"

// Persistent-block chunks segment file format.
//
// A block's `chunks/` directory holds one or more segment files:
//
//   chunks/
//     000001   <- 6-digit zero-padded, starts at 1
//     000002
//     ...
//
// Layout of one segment file:
//   magic            : 4 bytes BE (0x85BD40DD)
//   version          : 1 byte (== 1)
//   padding          : 3 bytes (zero)
//   per chunk, repeated until EOF:
//     length         : uvarint of chunk_data.size()
//     encoding       : 1 byte (chunkenc::Encoding)
//     data           : `length` bytes (raw XORChunk bytes incl. 2-byte sample count header)
//     crc32          : 4 bytes BE (Castagnoli, over encoding byte + data)
//
// A BlockChunkRef is a packed (seq << 32 | offset) where `offset` points to
// the START of the length uvarint, NOT to the data — matches Go's
// chunks/chunks.go ChunkOrIterable semantics.
//
// Reference: prometheus/tsdb/chunks/chunks.go lines 32-50, 247-257, 711-749.
namespace merlion_tsdb::block {

inline constexpr std::uint32_t k_magic_chunks                  = 0x85BD40DDU;
inline constexpr std::uint8_t  k_chunks_format_v1              = 1;
inline constexpr std::size_t   k_chunks_segment_header_size    = 8;  // 4 + 1 + 3
inline constexpr std::size_t   k_chunk_encoding_size           = 1;
inline constexpr std::size_t   k_chunk_crc_size                = 4;
inline constexpr std::size_t   k_chunks_segment_filename_width = 6;
inline constexpr std::size_t   k_default_chunks_segment_size   = 512U * 1024U * 1024U;  // 512 MiB

struct BlockChunkRef {
    std::uint32_t seq    = 0;   // segment file index (matches the numeric filename)
    std::uint32_t offset = 0;   // byte offset of the length uvarint within that segment

    [[nodiscard]] static constexpr BlockChunkRef from_u64(std::uint64_t v) noexcept {
        return {static_cast<std::uint32_t>(v >> 32),
                static_cast<std::uint32_t>(v & 0xFFFFFFFFU)};
    }
    [[nodiscard]] constexpr std::uint64_t to_u64() const noexcept {
        return (static_cast<std::uint64_t>(seq) << 32) |
                static_cast<std::uint64_t>(offset);
    }
    friend bool operator==(const BlockChunkRef&, const BlockChunkRef&) = default;
};

// One chunk's decoded payload — the encoding tag from the segment header
// plus the raw bytes that can be fed to `chunkenc::XORChunk::from_bytes`
// (or the equivalent for histograms once those are ported).
struct ChunkPayload {
    chunkenc::Encoding   encoding;
    std::vector<std::uint8_t> data;
    friend bool operator==(const ChunkPayload&, const ChunkPayload&) = default;
};

// Writes chunks into one or more segment files under `<dir>/chunks/`.
class ChunkWriter {
public:
    [[nodiscard]] static std::expected<ChunkWriter, std::error_code>
    open(const std::filesystem::path& dir,
         std::size_t segment_size = k_default_chunks_segment_size);

    ChunkWriter(const ChunkWriter&) = delete;
    ChunkWriter& operator=(const ChunkWriter&) = delete;
    ChunkWriter(ChunkWriter&&) noexcept;
    ChunkWriter& operator=(ChunkWriter&&) noexcept;
    ~ChunkWriter();

    // Append one chunk. Cuts a new segment if the chunk wouldn't fit in
    // the current one. Returns the ref for the index to record.
    [[nodiscard]] std::expected<BlockChunkRef, std::error_code>
    write(chunkenc::Encoding enc, std::span<const std::uint8_t> data);

    // fsync + close the current segment.
    [[nodiscard]] std::expected<void, std::error_code> close();

    [[nodiscard]] std::uint32_t current_seq() const noexcept { return seq_; }

private:
    ChunkWriter(std::filesystem::path dir, std::size_t segment_size) noexcept;

    [[nodiscard]] std::expected<void, std::error_code> cut_new_segment();
    [[nodiscard]] std::expected<void, std::error_code> write_all(std::span<const std::uint8_t>);

    std::filesystem::path dir_;
    std::size_t           segment_size_;
    int                   fd_         = -1;
    std::uint32_t         seq_        = 0;
    std::uint64_t         seg_offset_ = 0;  // bytes written into the current segment
    bool                  closed_     = false;
};

// Reads chunks from `<dir>/chunks/`. Loads each segment into memory on
// first access; mmap-backed reads are a follow-up.
class ChunkReader {
public:
    [[nodiscard]] static std::expected<ChunkReader, std::error_code>
    open(const std::filesystem::path& dir);

    [[nodiscard]] std::expected<ChunkPayload, std::error_code>
    read(BlockChunkRef ref) const;

    // Iterate every chunk in segment `seq` in stored order. Primarily for
    // tests / debugging; the index TOC is the production iteration source.
    struct ChunkInfo {
        BlockChunkRef       ref;
        chunkenc::Encoding  encoding;
        std::size_t         data_size;
    };
    [[nodiscard]] std::expected<std::vector<ChunkInfo>, std::error_code>
    iterate_segment(std::uint32_t seq) const;

    [[nodiscard]] std::size_t segment_count() const noexcept {
        return segments_.size();
    }
    [[nodiscard]] std::vector<std::uint32_t> segment_seqs() const;

private:
    explicit ChunkReader(std::vector<std::pair<std::uint32_t, std::filesystem::path>> segs) noexcept
        : segments_(std::move(segs)) {}

    [[nodiscard]] std::expected<std::span<const std::uint8_t>, std::error_code>
    bytes_for(std::uint32_t seq) const;

    std::vector<std::pair<std::uint32_t, std::filesystem::path>> segments_;
    // mutable so const read() can lazy-load. Indexed by `seq`.
    mutable std::vector<std::vector<std::uint8_t>> segment_bytes_cache_;
};

}  // namespace merlion_tsdb::block
