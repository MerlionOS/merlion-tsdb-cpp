#include "merlion_tsdb/block/block.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "merlion_tsdb/block/index_writer.hpp"
#include "merlion_tsdb/block/ulid.hpp"

namespace merlion_tsdb::block {

Block::Block(std::filesystem::path dir,
             BlockMeta meta,
             IndexReader index,
             ChunkReader chunks) noexcept
    : dir_(std::move(dir)),
      meta_(std::move(meta)),
      index_(std::move(index)),
      chunks_(std::move(chunks)) {}

std::expected<Block, std::error_code>
Block::open(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (ec) return std::unexpected(ec);
        return std::unexpected(std::make_error_code(std::errc::not_a_directory));
    }

    auto meta_or = read_meta(dir);
    if (!meta_or) return std::unexpected(meta_or.error());

    auto index_or = IndexReader::open(dir / "index");
    if (!index_or) return std::unexpected(index_or.error());

    auto chunks_or = ChunkReader::open(dir / "chunks");
    if (!chunks_or) return std::unexpected(chunks_or.error());

    return Block{dir,
                 std::move(*meta_or),
                 std::move(*index_or),
                 std::move(*chunks_or)};
}

std::expected<std::filesystem::path, std::error_code>
Block::create_from_series(const std::filesystem::path& parent_dir,
                          std::span<const SeriesInput> series) {
    std::error_code ec;
    std::filesystem::create_directories(parent_dir, ec);
    if (ec) return std::unexpected(ec);

    const auto ulid = new_ulid();
    const auto block_dir = parent_dir / ulid;
    if (std::filesystem::exists(block_dir, ec)) {
        return std::unexpected(std::make_error_code(std::errc::file_exists));
    }
    if (ec) return std::unexpected(ec);
    std::filesystem::create_directories(block_dir, ec);
    if (ec) return std::unexpected(ec);

    // -------- Step 1: write all chunks via ChunkWriter ---------------------
    // For each input series, record the ChunkMeta list with the assigned refs.
    struct PreparedSeries {
        const SeriesInput*           input;
        std::vector<ChunkMeta>       chunk_metas;
    };
    std::vector<PreparedSeries> prepared;
    prepared.reserve(series.size());

    std::uint64_t total_samples = 0;
    std::uint64_t total_chunks  = 0;
    std::int64_t  min_time = std::numeric_limits<std::int64_t>::max();
    std::int64_t  max_time = std::numeric_limits<std::int64_t>::min();

    {
        auto cw_or = ChunkWriter::open(block_dir / "chunks");
        if (!cw_or) return std::unexpected(cw_or.error());
        auto& cw = *cw_or;

        for (const auto& s : series) {
            if (s.chunks.empty()) continue;
            PreparedSeries ps;
            ps.input = &s;
            ps.chunk_metas.reserve(s.chunks.size());

            for (const auto& chk : s.chunks) {
                auto ref = cw.write(chunkenc::Encoding::XOR, chk.bytes);
                if (!ref) return std::unexpected(ref.error());
                ChunkMeta meta;
                meta.min_time = chk.min_time;
                meta.max_time = chk.max_time;
                meta.ref      = ref->to_u64();
                ps.chunk_metas.push_back(meta);

                if (chk.min_time < min_time) min_time = chk.min_time;
                if (chk.max_time > max_time) max_time = chk.max_time;

                // Sample count is encoded in the first 2 bytes of the
                // XOR chunk's body, big-endian.
                if (chk.bytes.size() >= 2) {
                    const std::uint16_t n =
                        static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(chk.bytes[0]) << 8) |
                             static_cast<std::uint16_t>(chk.bytes[1]));
                    total_samples += static_cast<std::uint64_t>(n);
                }
                ++total_chunks;
            }
            prepared.push_back(std::move(ps));
        }
        if (auto r = cw.close(); !r) return std::unexpected(r.error());
    }

    // Empty input is legal; meta.json reflects zero series/samples.
    if (prepared.empty()) {
        min_time = 0;
        max_time = 0;
    }

    // -------- Step 2: build the symbol table -------------------------------
    // Sorted, deduplicated set of every label name + label value plus the
    // empty string (used by upstream's posting offset table for the "all
    // postings" sentinel).
    std::set<std::string> symbols;
    symbols.insert("");
    for (const auto& ps : prepared) {
        for (const auto& l : ps.input->labels.entries()) {
            symbols.insert(l.name);
            symbols.insert(l.value);
        }
    }

    // Map symbol → its IndexWriter ref (0-indexed assignment in sorted order).
    std::unordered_map<std::string, std::uint32_t> sym_ref;
    sym_ref.reserve(symbols.size());

    // -------- Step 3: open IndexWriter and emit -----------------------------
    auto iw_or = IndexWriter::create(block_dir / "index");
    if (!iw_or) return std::unexpected(iw_or.error());
    auto& iw = *iw_or;

    for (const auto& s : symbols) {
        auto ref = iw.add_symbol(s);
        if (!ref) return std::unexpected(ref.error());
        sym_ref.emplace(s, *ref);
    }
    if (auto r = iw.finish_symbols(); !r) return std::unexpected(r.error());

    // Build (name, value) → posting list. Inserting series in the input
    // order assigns ascending series IDs, which keeps the posting lists
    // sorted by id without any extra work.
    std::map<std::pair<std::string, std::string>, std::vector<std::uint32_t>> postings;

    for (auto& ps : prepared) {
        // Translate labels to (name_ref, value_ref) pairs.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> label_refs;
        label_refs.reserve(ps.input->labels.size());
        for (const auto& l : ps.input->labels.entries()) {
            label_refs.emplace_back(sym_ref.at(l.name), sym_ref.at(l.value));
        }
        auto series_id = iw.add_series(label_refs, ps.chunk_metas);
        if (!series_id) return std::unexpected(series_id.error());

        for (const auto& l : ps.input->labels.entries()) {
            postings[{l.name, l.value}].push_back(*series_id);
        }
    }
    if (auto r = iw.finish_series(); !r) return std::unexpected(r.error());

    for (const auto& [key, ids] : postings) {
        if (auto r = iw.add_postings(key.first, key.second, ids); !r) {
            return std::unexpected(r.error());
        }
    }
    if (auto r = iw.close(); !r) return std::unexpected(r.error());

    // -------- Step 4: meta.json --------------------------------------------
    BlockMeta meta;
    meta.version  = 1;
    meta.ulid     = ulid;
    meta.min_time = min_time;
    meta.max_time = max_time;
    meta.stats.num_samples = total_samples;
    meta.stats.num_series  = prepared.size();
    meta.stats.num_chunks  = total_chunks;
    meta.compaction.level   = 1;
    meta.compaction.sources = {ulid};

    if (auto r = write_meta(block_dir, meta); !r) {
        return std::unexpected(r.error());
    }

    // -------- Step 5: empty tombstones file --------------------------------
    {
        std::ofstream out(block_dir / "tombstones", std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    return block_dir;
}

std::expected<std::vector<Block::QueryResult>, std::error_code>
Block::query(std::string_view name, std::string_view value) const {
    auto ids_or = index_.postings(name, value);
    if (!ids_or) return std::unexpected(ids_or.error());

    std::vector<QueryResult> out;
    out.reserve(ids_or->size());
    for (auto id : *ids_or) {
        auto series_or = index_.series(id);
        if (!series_or) return std::unexpected(series_or.error());

        QueryResult qr;
        qr.labels      = std::move(series_or->labels);
        qr.chunk_metas = std::move(series_or->chunks);
        qr.chunks.reserve(qr.chunk_metas.size());
        for (const auto& cm : qr.chunk_metas) {
            auto payload = chunks_.read(cm.chunk_ref());
            if (!payload) return std::unexpected(payload.error());
            // For MVP only XOR-encoded chunks are decoded; histogram /
            // float-histogram chunks can be added in a follow-up that
            // ports those encoders.
            if (payload->encoding != chunkenc::Encoding::XOR) {
                return std::unexpected(std::make_error_code(std::errc::not_supported));
            }
            qr.chunks.push_back(
                chunkenc::XORChunk::from_bytes(std::move(payload->data)));
        }
        out.push_back(std::move(qr));
    }
    return out;
}

}  // namespace merlion_tsdb::block
