#include "merlion_tsdb/head/mem_series.hpp"

#include <utility>

namespace merlion_tsdb::head {

MemSeries::MemSeries(SeriesRef ref, model::Labels labels)
    : ref_(ref), labels_(std::move(labels)) {
    // Lazily allocate the first chunk on first append. This keeps a
    // freshly-replayed Series record from costing 128+ bytes of empty
    // chunk memory when no samples land for it.
}

void MemSeries::cut_chunk() {
    current_appender_.reset();
    chunks_.push_back(std::make_unique<chunkenc::XORChunk>());
    auto app = chunks_.back()->appender();
    // appender() on an empty chunk cannot fail — it short-circuits to a
    // fresh state without iterating. If it ever does (e.g., XOR2 added
    // a validation step), surfacing the error needs a different ctor
    // signature; for MVP, treat as a programmer error.
    if (app) current_appender_.emplace(std::move(*app));
}

bool MemSeries::append(std::int64_t t, double v) {
    // Monotonic timestamps required. Upstream returns ErrOutOfOrderSample;
    // we just refuse silently and let the caller observe via last_t().
    if (has_sample_ && t < last_t_) return false;

    if (chunks_.empty() ||
        chunks_.back()->bytes().size() > k_max_bytes_per_chunk_before_append) {
        cut_chunk();
    }

    if (!current_appender_->append(t, v)) {
        // Hit the u16 sample-count ceiling on this chunk. Cut once, retry.
        cut_chunk();
        if (!current_appender_->append(t, v)) {
            return false;  // pathological — would only happen if cut() left an empty chunk that still couldn't accept
        }
    }

    last_t_ = t;
    last_v_ = v;
    ++num_samples_;
    has_sample_ = true;
    return true;
}

std::vector<const chunkenc::XORChunk*> MemSeries::chunks() const {
    std::vector<const chunkenc::XORChunk*> out;
    out.reserve(chunks_.size());
    for (const auto& c : chunks_) out.push_back(c.get());
    return out;
}

void MemSeries::reset() {
    current_appender_.reset();
    chunks_.clear();
    last_t_      = std::numeric_limits<std::int64_t>::min();
    last_v_      = 0.0;
    num_samples_ = 0;
    has_sample_  = false;
}

}  // namespace merlion_tsdb::head
