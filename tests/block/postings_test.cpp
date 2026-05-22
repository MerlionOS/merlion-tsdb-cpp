#include "merlion_tsdb/block/index.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace b = merlion_tsdb::block;

namespace {

std::filesystem::path golden_block_dir() {
    auto p = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        const auto candidate = p / "testdata" / "index_format_v1";
        if (std::filesystem::exists(candidate / "meta.json")) return candidate;
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    ADD_FAILURE() << "could not locate testdata/index_format_v1/ above CWD "
                  << std::filesystem::current_path();
    return {};
}

}  // namespace

TEST(PostingsOffsetTable, GoldenTableIsNonEmpty) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto& pot = r.postings_table();
    EXPECT_GT(pot.size(), 0u);
}

TEST(PostingsOffsetTable, GoldenHasExpectedLabelNames) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto& entries = r.postings_table().entries();
    std::set<std::string> names;
    for (const auto& e : entries) names.insert(e.name);
    // The V1 fixture only uses "bar" and "foo" as label NAMES; "baz" is a
    // VALUE (paired with "foo") and "meh" appears only as a free-floating
    // symbol. Upstream does NOT pre-sort the offset table — entries are
    // written in whatever iteration order the writer chose.
    EXPECT_TRUE(names.count("bar") > 0);
    EXPECT_TRUE(names.count("foo") > 0);
    EXPECT_FALSE(names.count("baz") > 0);
    EXPECT_FALSE(names.count("meh") > 0);
}

TEST(PostingsOffsetTable, LookupReturnsOffsetForExistingPair) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto& pot = r.postings_table();
    // Pick the first entry and look it up — should match.
    const auto& first = pot.entries().front();
    auto off = pot.lookup(first.name, first.value);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, first.offset);
}

TEST(PostingsOffsetTable, LookupReturnsEmptyForMissingPair) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    auto off = r.postings_table().lookup("nonexistent_label", "value");
    EXPECT_FALSE(off.has_value());
}

TEST(PostingList, ReadsListsForGoldenBlock) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto& entries = r.postings_table().entries();
    ASSERT_GT(entries.size(), 0u);

    // Every offset must point at a valid posting list. Decode each one and
    // verify the series refs are in a plausible range. The golden block has
    // 102 series so series refs are bounded above by some moderate value
    // (Go writes series-refs in a deterministic order; we don't know the
    // exact range without parsing the series section, so just check
    // sortedness within a list).
    std::size_t total_refs = 0;
    for (const auto& e : entries) {
        auto refs = b::read_posting_list(r.bytes(), e.offset);
        ASSERT_TRUE(refs.has_value())
            << "failed reading postings for " << e.name << "=" << e.value;
        // A posting list is sorted ascending.
        for (std::size_t i = 1; i < refs->size(); ++i) {
            EXPECT_LT((*refs)[i - 1], (*refs)[i])
                << "posting list for " << e.name << "=" << e.value
                << " not sorted at index " << i;
        }
        total_refs += refs->size();
    }
    EXPECT_GT(total_refs, 0u);
}

TEST(PostingList, ConvenienceReaderViaIndexReader) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    // Pick a label pair we know exists (first entry in the table).
    const auto& first = r.postings_table().entries().front();
    auto via_convenience = r.postings(first.name, first.value);
    auto via_direct      = b::read_posting_list(r.bytes(), first.offset);
    ASSERT_TRUE(via_convenience.has_value());
    ASSERT_TRUE(via_direct.has_value());
    EXPECT_EQ(*via_convenience, *via_direct);
}

TEST(PostingList, ConvenienceReturnsEmptyForUnknownLabel) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    auto refs = r.postings("not_a_real_label", "any_value");
    ASSERT_TRUE(refs.has_value());
    EXPECT_TRUE(refs->empty());
}

TEST(PostingList, EveryGoldenSeriesAppearsInAtLeastOnePosting) {
    // The "" => "" entry (or some sentinel) is the "all postings" — must
    // contain every series ref. Find it and assert membership.
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto& entries = r.postings_table().entries();
    auto it = std::find_if(entries.begin(), entries.end(),
                           [](const b::PostingsOffsetEntry& e) {
                               return e.name.empty() && e.value.empty();
                           });
    if (it == entries.end()) {
        GTEST_SKIP() << "fixture doesn't carry an empty label pair; skip";
    }
    auto refs = b::read_posting_list(r.bytes(), it->offset);
    ASSERT_TRUE(refs.has_value());
    // For the 102-series golden block.
    EXPECT_EQ(refs->size(), 102u);
}
