#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A label set: ordered, deduplicated, hashable. The same in spirit as Go's
// `model/labels.Labels` but with C++-idiomatic ergonomics:
//
//   - Labels are always stored sorted by name. The canonical order is
//     established at construction and never changes thereafter.
//   - Duplicate names are not allowed; later entries silently override
//     earlier ones at construction time (matches the LabelsBuilder.Set
//     idiom in upstream Go).
//   - `hash()` produces a 64-bit value usable as a `unordered_map` key.
//     The hash is intentionally **not** byte-compatible with Go's
//     `labels.Hash` (which is xxhash-based); we only need it to be
//     deterministic across runs of the same impl, since label sets never
//     hit the on-disk format directly. The wire format stores labels as
//     UTF-8 name/value pairs (see wal::record::Series).
namespace merlion_tsdb::model {

struct Label {
    std::string name;
    std::string value;

    friend bool          operator==(const Label&, const Label&) = default;
    friend std::strong_ordering operator<=>(const Label&, const Label&) = default;
};

class Labels {
public:
    Labels() = default;

    // Construct from a list of (possibly unsorted, possibly duplicate-named)
    // labels. Sorts by name; on duplicate names, keeps the last value.
    Labels(std::initializer_list<Label> entries);
    explicit Labels(std::vector<Label> entries);

    [[nodiscard]] std::span<const Label> entries() const noexcept {
        return {entries_.data(), entries_.size()};
    }
    [[nodiscard]] bool        empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size()  const noexcept { return entries_.size(); }

    // Returns the value for `name`, or empty optional if not present.
    [[nodiscard]] std::optional<std::string_view>
    get(std::string_view name) const noexcept;

    // Returns true iff `name` is present.
    [[nodiscard]] bool has(std::string_view name) const noexcept {
        return get(name).has_value();
    }

    // Deterministic 64-bit hash. Stable across calls on equal Labels.
    [[nodiscard]] std::uint64_t hash() const noexcept;

    friend bool operator==(const Labels&, const Labels&) = default;

    // Builder-style mutation. Re-sorts / de-dupes on each call so callers
    // can chain without thinking about ordering. Returns *this for chaining.
    Labels& add(std::string name, std::string value);

private:
    void canonicalize();  // sort by name, dedupe (keep last)

    std::vector<Label> entries_;  // sorted by name; no duplicate names
};

// Hash functor adaptor for use in standard associative containers.
struct LabelsHash {
    [[nodiscard]] std::size_t operator()(const Labels& l) const noexcept {
        return static_cast<std::size_t>(l.hash());
    }
};

}  // namespace merlion_tsdb::model
