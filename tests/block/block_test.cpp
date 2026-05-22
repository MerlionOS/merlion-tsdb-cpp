#include "merlion_tsdb/block/block.hpp"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace b = merlion_tsdb::block;
namespace m = merlion_tsdb::model;
namespace c = merlion_tsdb::chunkenc;

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

TEST(Block, OpensGoldenBlock) {
    auto blk_or = b::Block::open(golden_block_dir());
    ASSERT_TRUE(blk_or.has_value())
        << "open failed: " << blk_or.error().message();
    const auto& m = blk_or->meta();
    EXPECT_EQ(m.version, 1);
    EXPECT_EQ(m.ulid, "01DXXFZDYD1MQW6079WK0K6EDQ");
    EXPECT_EQ(m.stats.num_series, 102u);
    EXPECT_EQ(m.stats.num_chunks, 102u);
    EXPECT_EQ(blk_or->chunks().segment_count(), 1u);
}

TEST(Block, NonExistentDirectoryReturnsError) {
    auto r = b::Block::open("/tmp/this/should/not/exist/merlion-block-test");
    EXPECT_FALSE(r.has_value());
}

TEST(Block, QueryByExistingFooBazReturnsOneSeries) {
    auto blk = *b::Block::open(golden_block_dir());
    // The synthetic fixture has exactly one series carrying foo=baz.
    auto results = blk.query("foo", "baz");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    const auto& r = (*results)[0];
    // The result's labels include foo=baz.
    auto v = r.labels.get("foo");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "baz");
    // Has at least one chunk.
    ASSERT_FALSE(r.chunks.empty());
}

TEST(Block, QueryByMissingLabelReturnsEmpty) {
    auto blk = *b::Block::open(golden_block_dir());
    auto results = blk.query("nonexistent", "value");
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

TEST(Block, QueryByBarValueReturnsMatchingSeries) {
    auto blk = *b::Block::open(golden_block_dir());
    // Pick an arbitrary bar=N (N in 0..99 should exist in the V1 fixture).
    auto results = blk.query("bar", "42");
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_GE(results->size(), 1u);
    for (const auto& r : *results) {
        auto v = r.labels.get("bar");
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, "42");
    }
}

TEST(Block, QueryDecodedChunksAreIterable) {
    auto blk = *b::Block::open(golden_block_dir());
    auto results = blk.query("foo", "baz");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    const auto& r = (*results)[0];
    ASSERT_FALSE(r.chunks.empty());

    // Every decoded chunk must contain at least one sample and iterate
    // without error. The synthetic fixture uses trivial sample data so
    // we don't assert exact values — just decode shape.
    std::size_t total_samples = 0;
    for (const auto& chunk : r.chunks) {
        EXPECT_GT(chunk.num_samples(), 0);
        auto it = chunk.iterator();
        while (it.next()) ++total_samples;
        EXPECT_FALSE(it.error().has_value());
    }
    EXPECT_GT(total_samples, 0u);
}

TEST(Block, EveryGoldenSeriesIsReachableViaSomePosting) {
    // Aggregate query: walk every (name, value) in the postings table,
    // query for it, and union the result labels' implied series counts.
    // All 102 fixture series should be discoverable this way.
    auto blk = *b::Block::open(golden_block_dir());
    std::set<std::string> seen_label_strings;
    for (const auto& entry : blk.index().postings_table().entries()) {
        if (entry.name.empty() && entry.value.empty()) continue;
        auto results = blk.query(entry.name, entry.value);
        ASSERT_TRUE(results.has_value()) << "query failed for "
            << entry.name << "=" << entry.value;
        for (const auto& r : *results) {
            // Build a stable string from the label set.
            std::string s;
            for (const auto& l : r.labels.entries()) {
                if (!s.empty()) s += ",";
                s += l.name + "=" + l.value;
            }
            seen_label_strings.insert(std::move(s));
        }
    }
    EXPECT_EQ(seen_label_strings.size(), 102u);
}

TEST(Block, ChunkMetaTimeRangesMatchQueryRange) {
    auto blk = *b::Block::open(golden_block_dir());
    auto results = blk.query("foo", "baz");
    ASSERT_TRUE(results.has_value());
    for (const auto& r : *results) {
        for (const auto& meta : r.chunk_metas) {
            EXPECT_LE(meta.min_time, meta.max_time);
            // Must be within the block's overall time range.
            EXPECT_GE(meta.min_time, blk.meta().min_time);
            EXPECT_LE(meta.max_time, blk.meta().max_time);
        }
    }
}
