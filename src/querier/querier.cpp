#include "merlion_tsdb/querier/querier.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace merlion_tsdb::querier {

Querier::Querier(std::span<const block::Block* const> blocks)
    : blocks_(blocks.begin(), blocks.end()) {}

std::expected<std::vector<MergedSeries>, std::error_code>
Querier::select(std::span<const model::Matcher> matchers,
                std::int64_t mint,
                std::int64_t maxt) const {
    if (matchers.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    // Hash-bucket merge keyed on Labels. Each bucket holds the running
    // (chunks, chunk_metas) parallel pair; chunks from later blocks are
    // appended and the final result is sorted by min_time per bucket.
    std::unordered_map<model::Labels, MergedSeries, model::LabelsHash> bucket;

    for (const auto* blk : blocks_) {
        if (!blk) continue;
        // Block-level time-range short-circuit: meta covers
        // [min_time, max_time], the inclusive overlap test matches
        // §8.1.
        const auto& meta = blk->meta();
        if (meta.min_time > maxt || meta.max_time < mint) continue;

        auto per_block = blk->select(matchers, mint, maxt);
        if (!per_block) return std::unexpected(per_block.error());

        for (auto& qr : *per_block) {
            auto [it, inserted] = bucket.try_emplace(qr.labels);
            if (inserted) {
                it->second.labels = qr.labels;
            }
            auto& dst = it->second;
            for (std::size_t i = 0; i < qr.chunks.size(); ++i) {
                dst.chunks.push_back(std::move(qr.chunks[i]));
                dst.chunk_metas.push_back(qr.chunk_metas[i]);
            }
        }
    }

    std::vector<MergedSeries> out;
    out.reserve(bucket.size());
    for (auto& [_, series] : bucket) {
        // Sort chunks by min_time ascending. We sort an index vector
        // and apply it to both parallel arrays so XORChunk (non-
        // trivially-copyable) only moves once.
        const auto n = series.chunks.size();
        std::vector<std::size_t> order(n);
        for (std::size_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return series.chunk_metas[a].min_time <
                             series.chunk_metas[b].min_time;
                  });
        std::vector<chunkenc::XORChunk>     sorted_chunks;
        std::vector<block::ChunkMeta>       sorted_metas;
        sorted_chunks.reserve(n);
        sorted_metas.reserve(n);
        for (auto i : order) {
            sorted_chunks.push_back(std::move(series.chunks[i]));
            sorted_metas.push_back(series.chunk_metas[i]);
        }
        series.chunks      = std::move(sorted_chunks);
        series.chunk_metas = std::move(sorted_metas);
        out.push_back(std::move(series));
    }
    return out;
}

}  // namespace merlion_tsdb::querier
