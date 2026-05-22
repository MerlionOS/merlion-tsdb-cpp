#include "merlion_tsdb/model/labels.hpp"

#include <algorithm>
#include <utility>

namespace merlion_tsdb::model {

namespace {

// FNV-1a 64-bit. Cheap, well-distributed, deterministic, and gives us
// stable hashes without dragging in xxhash. Not for cryptographic use.
// Wire format never sees this — labels are stored as raw UTF-8.
constexpr std::uint64_t k_fnv_offset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t k_fnv_prime  = 0x00000100000001B3ULL;

std::uint64_t fnv1a(std::uint64_t h, std::string_view s) noexcept {
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= k_fnv_prime;
    }
    return h;
}

// Separator byte between label fields in the hash stream. Different from
// any UTF-8 byte that could appear naturally, so "ab" / "c" and "a" / "bc"
// can't collide.
constexpr unsigned char k_sep_field = 0xFF;
constexpr unsigned char k_sep_pair  = 0xFE;

}  // namespace

Labels::Labels(std::initializer_list<Label> entries) : entries_(entries) {
    canonicalize();
}

Labels::Labels(std::vector<Label> entries) : entries_(std::move(entries)) {
    canonicalize();
}

void Labels::canonicalize() {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Label& a, const Label& b) {
                         return a.name < b.name;
                     });
    // Dedupe by name, keeping the LAST occurrence. std::unique keeps the
    // first, so iterate manually.
    if (entries_.empty()) return;
    std::vector<Label> out;
    out.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (i + 1 < entries_.size() && entries_[i].name == entries_[i + 1].name) {
            continue;  // skip; later duplicate will be kept
        }
        out.push_back(std::move(entries_[i]));
    }
    entries_ = std::move(out);
}

std::optional<std::string_view>
Labels::get(std::string_view name) const noexcept {
    // Binary search since entries_ is sorted by name.
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), name,
        [](const Label& l, std::string_view n) { return l.name < n; });
    if (it == entries_.end() || it->name != name) return std::nullopt;
    return std::string_view(it->value);
}

std::uint64_t Labels::hash() const noexcept {
    std::uint64_t h = k_fnv_offset;
    for (const auto& l : entries_) {
        h = fnv1a(h, l.name);
        h ^= k_sep_field; h *= k_fnv_prime;
        h = fnv1a(h, l.value);
        h ^= k_sep_pair;  h *= k_fnv_prime;
    }
    return h;
}

Labels& Labels::add(std::string name, std::string value) {
    entries_.push_back({std::move(name), std::move(value)});
    canonicalize();
    return *this;
}

}  // namespace merlion_tsdb::model
