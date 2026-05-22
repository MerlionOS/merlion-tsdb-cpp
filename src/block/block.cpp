#include "merlion_tsdb/block/block.hpp"

#include <utility>

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
