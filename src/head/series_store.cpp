#include "merlion_tsdb/head/series_store.hpp"

#include <algorithm>
#include <utility>

namespace merlion_tsdb::head {

namespace {

// Make sure `by_ref_` is big enough that index `ref - 1` is valid.
void ensure_ref_slot(std::vector<std::unique_ptr<MemSeries>>& by_ref,
                     SeriesRef ref) {
    if (ref == 0) return;  // reserved
    if (ref - 1 >= by_ref.size()) by_ref.resize(static_cast<std::size_t>(ref));
}

}  // namespace

std::pair<MemSeries*, bool>
SeriesStore::get_or_create(const model::Labels& labels) {
    if (auto it = by_labels_.find(labels); it != by_labels_.end()) {
        return {by_ref_[static_cast<std::size_t>(it->second) - 1].get(), false};
    }
    const SeriesRef ref = next_ref_++;
    ensure_ref_slot(by_ref_, ref);
    by_ref_[static_cast<std::size_t>(ref) - 1] =
        std::make_unique<MemSeries>(ref, labels);
    by_labels_.emplace(labels, ref);
    return {by_ref_[static_cast<std::size_t>(ref) - 1].get(), true};
}

MemSeries* SeriesStore::get(SeriesRef ref) noexcept {
    if (ref == 0 || ref - 1 >= by_ref_.size()) return nullptr;
    return by_ref_[static_cast<std::size_t>(ref) - 1].get();
}

const MemSeries* SeriesStore::get(SeriesRef ref) const noexcept {
    if (ref == 0 || ref - 1 >= by_ref_.size()) return nullptr;
    return by_ref_[static_cast<std::size_t>(ref) - 1].get();
}

bool SeriesStore::insert_with_ref(SeriesRef ref, model::Labels labels) {
    if (ref == 0) return false;

    // If the ref already exists, it must match the supplied labels.
    if (ref - 1 < by_ref_.size() && by_ref_[ref - 1]) {
        return by_ref_[ref - 1]->labels() == labels;
    }
    // If the labels already exist under a different ref, refuse.
    if (auto it = by_labels_.find(labels); it != by_labels_.end()) {
        return it->second == ref;
    }

    ensure_ref_slot(by_ref_, ref);
    by_ref_[ref - 1] = std::make_unique<MemSeries>(ref, std::move(labels));
    by_labels_.emplace(by_ref_[ref - 1]->labels(), ref);
    next_ref_ = std::max(next_ref_, ref + 1);
    return true;
}

std::vector<MemSeries*> SeriesStore::all_series() {
    std::vector<MemSeries*> out;
    out.reserve(by_ref_.size());
    for (auto& s : by_ref_) if (s) out.push_back(s.get());
    return out;
}

std::vector<const MemSeries*> SeriesStore::all_series() const {
    std::vector<const MemSeries*> out;
    out.reserve(by_ref_.size());
    for (const auto& s : by_ref_) if (s) out.push_back(s.get());
    return out;
}

}  // namespace merlion_tsdb::head
