#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "merlion_tsdb/head/mem_series.hpp"
#include "merlion_tsdb/model/labels.hpp"

// Single-threaded series store: maps labels → MemSeries* and ref → MemSeries*.
//
// Refs are u64, dispensed monotonically starting at 1 (0 is reserved as
// "no ref"). Once assigned, a ref is immutable for the lifetime of the
// store — this is the load-bearing invariant relied on by the WAL replay
// (every Series record fixes a ref that subsequent Samples records
// reference by delta).
//
// MVP is single-threaded. Stripe locking and concurrent appenders are
// future work.
namespace merlion_tsdb::head {

class SeriesStore {
public:
    // Either returns the existing MemSeries* for `labels`, or creates a new
    // one with the next available ref. The bool is true iff the series was
    // newly created (caller must emit a WAL Series record in that case).
    std::pair<MemSeries*, bool> get_or_create(const model::Labels& labels);

    // Look up by ref. Returns nullptr if no such ref exists.
    [[nodiscard]] MemSeries* get(SeriesRef ref) noexcept;
    [[nodiscard]] const MemSeries* get(SeriesRef ref) const noexcept;

    // Inserts a MemSeries with a specific ref — used during WAL replay
    // when the on-disk Series record dictates the (ref, labels) pair.
    // Returns false if the ref or labels are already in use with a
    // different mapping. Updates `next_ref_` to be greater than `ref`.
    bool insert_with_ref(SeriesRef ref, model::Labels labels);

    [[nodiscard]] std::size_t size() const noexcept { return by_ref_.size(); }

    // Snapshot of all series. Iteration order is by ref (ascending).
    [[nodiscard]] std::vector<MemSeries*> all_series();
    [[nodiscard]] std::vector<const MemSeries*> all_series() const;

    // Returns the next ref that would be assigned by get_or_create. Useful
    // for tests; production code should not depend on this.
    [[nodiscard]] SeriesRef next_ref() const noexcept { return next_ref_; }

private:
    // ref → MemSeries (owning). Indexed densely by ref - 1.
    std::vector<std::unique_ptr<MemSeries>> by_ref_;
    // labels → ref (so we can look up an existing series without re-hashing
    // its chunks).
    std::unordered_map<model::Labels, SeriesRef, model::LabelsHash> by_labels_;
    SeriesRef next_ref_ = 1;
};

}  // namespace merlion_tsdb::head
