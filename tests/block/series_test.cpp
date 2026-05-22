#include "merlion_tsdb/block/index.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace b = merlion_tsdb::block;
namespace m = merlion_tsdb::model;

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

// Collect every distinct series ID from the index's postings.
std::set<std::uint32_t> all_series_ids(const b::IndexReader& r) {
    std::set<std::uint32_t> ids;
    for (const auto& entry : r.postings_table().entries()) {
        auto refs = b::read_posting_list(r.bytes(), entry.offset);
        if (!refs) continue;
        for (auto id : *refs) ids.insert(id);
    }
    return ids;
}

}  // namespace

TEST(SeriesReader, EveryGoldenSeriesDecodes) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto ids = all_series_ids(r);
    ASSERT_FALSE(ids.empty());
    // Golden block has 102 series.
    EXPECT_EQ(ids.size(), 102u);

    std::size_t total_chunks = 0;
    for (auto id : ids) {
        auto s = r.series(id);
        ASSERT_TRUE(s.has_value()) << "series id=" << id;
        // Every series has at least one label and one chunk.
        EXPECT_FALSE(s->labels.empty()) << "series id=" << id;
        EXPECT_FALSE(s->chunks.empty()) << "series id=" << id;
        total_chunks += s->chunks.size();
    }
    // meta.json says numChunks=102, so we should see exactly 102 chunks
    // total across all series.
    EXPECT_EQ(total_chunks, 102u);
}

TEST(SeriesReader, GoldenSeriesLabelsUseExpectedNamesOnly) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto ids = all_series_ids(r);
    std::set<std::string> names_seen;
    for (auto id : ids) {
        auto s = r.series(id);
        ASSERT_TRUE(s.has_value());
        for (const auto& l : s->labels.entries()) {
            names_seen.insert(l.name);
        }
    }
    // The V1 fixture only uses "bar" and "foo" as label names.
    EXPECT_EQ(names_seen, (std::set<std::string>{"bar", "foo"}));
}

TEST(SeriesReader, GoldenChunkRefsPointIntoChunksFile) {
    // KNOWN ISSUE: upstream's ChunkRef encodes `seq` as a 0-indexed array
    // position into the loaded segment list, NOT the filename's numeric
    // value. So the golden block's single chunks segment (filename
    // `000001`) is referenced as seq=0 here. The current block::ChunkReader
    // (phase 2) parses the filename and stores seq=1 in its key map,
    // which means an index-derived ChunkRef can't directly index it.
    // Tracked as a follow-up; this test pins the upstream convention.
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto ids = all_series_ids(r);
    for (auto id : ids) {
        auto s = r.series(id);
        ASSERT_TRUE(s.has_value());
        for (const auto& chk : s->chunks) {
            const auto cref = chk.chunk_ref();
            EXPECT_EQ(cref.seq, 0u)
                << "upstream encodes the first segment file as seq=0";
            // Offset must be past the segment header.
            EXPECT_GE(cref.offset, 8u);
        }
    }
}

TEST(SeriesReader, GoldenChunkTimesAreNonDecreasing) {
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    const auto ids = all_series_ids(r);
    for (auto id : ids) {
        auto s = r.series(id);
        ASSERT_TRUE(s.has_value());
        for (const auto& chk : s->chunks) {
            EXPECT_LE(chk.min_time, chk.max_time) << "id=" << id;
        }
        // Across chunks: min times must be ascending.
        for (std::size_t i = 1; i < s->chunks.size(); ++i) {
            EXPECT_LE(s->chunks[i - 1].max_time, s->chunks[i].min_time)
                << "series id=" << id << " chunk " << i;
        }
    }
}

TEST(SeriesReader, GoldenPostingsAndSeriesReferenceConsistently) {
    // Cross-check: for every label-pair in the postings table, the series
    // refs in its posting list should all have that (name, value) in their
    // own label sets. This catches off-by-one errors in either the
    // postings reader or the series reader.
    auto r = *b::IndexReader::open(golden_block_dir() / "index");
    for (const auto& e : r.postings_table().entries()) {
        if (e.name.empty() && e.value.empty()) continue;  // skip "all"
        auto refs = b::read_posting_list(r.bytes(), e.offset);
        ASSERT_TRUE(refs.has_value());
        for (auto id : *refs) {
            auto s = r.series(id);
            ASSERT_TRUE(s.has_value()) << "id=" << id;
            auto v = s->labels.get(e.name);
            ASSERT_TRUE(v.has_value())
                << "series id=" << id << " missing label " << e.name;
            EXPECT_EQ(*v, e.value)
                << "series id=" << id << " label " << e.name
                << " has value '" << *v << "' but is listed under '"
                << e.value << "' in the postings table";
        }
    }
}
