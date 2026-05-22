#include "merlion_tsdb/block/block.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/head/head.hpp"
#include "merlion_tsdb/model/labels.hpp"

namespace b = merlion_tsdb::block;
namespace h = merlion_tsdb::head;
namespace m = merlion_tsdb::model;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "merlion_cfh_%08x_%08x", rd(), rd());
        path_ = base / buf;
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

}  // namespace

TEST(CreateFromHead, EmptyHeadProducesEmptyBlock) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    auto dir_or = b::Block::create_from_head(tmp.path() / "blocks", head);
    ASSERT_TRUE(dir_or.has_value()) << dir_or.error().message();
    auto blk = *b::Block::open(*dir_or);
    EXPECT_EQ(blk.meta().stats.num_series, 0u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 0u);
    EXPECT_EQ(blk.meta().stats.num_samples, 0u);
}

TEST(CreateFromHead, SeriesAppendedToHeadAreInBlock) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    m::Labels la{{"__name__", "request_count"}, {"job", "api"}};
    m::Labels lb{{"__name__", "request_count"}, {"job", "web"}};

    ASSERT_TRUE(head.append(la, 100, 1.0).has_value());
    ASSERT_TRUE(head.append(la, 200, 2.0).has_value());
    ASSERT_TRUE(head.append(la, 300, 3.0).has_value());
    ASSERT_TRUE(head.append(lb, 100, 10.0).has_value());
    ASSERT_TRUE(head.append(lb, 200, 20.0).has_value());

    auto dir = *b::Block::create_from_head(tmp.path() / "blocks", head);
    auto blk = *b::Block::open(dir);

    EXPECT_EQ(blk.meta().stats.num_series, 2u);
    EXPECT_EQ(blk.meta().stats.num_samples, 5u);
    EXPECT_EQ(blk.meta().min_time, 100);
    EXPECT_EQ(blk.meta().max_time, 300);

    auto api = blk.query("job", "api");
    ASSERT_TRUE(api.has_value());
    ASSERT_EQ(api->size(), 1u);
    auto& a = (*api)[0];
    ASSERT_EQ(a.chunks.size(), 1u);
    std::vector<std::pair<std::int64_t, double>> got;
    {
        auto it = a.chunks[0].iterator();
        while (it.next()) got.emplace_back(it.t(), it.v());
    }
    EXPECT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].first, 100);
    EXPECT_DOUBLE_EQ(got[0].second, 1.0);
    EXPECT_EQ(got[2].first, 300);
    EXPECT_DOUBLE_EQ(got[2].second, 3.0);
}

TEST(CreateFromHead, MultiChunkSeriesPreservesEveryChunk) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    m::Labels lbl{{"__name__", "metric"}};

    // Force chunk cuts by appending random doubles. Each XOR chunk
    // soft-caps at 1 KiB; ~250 random samples reliably spans 2+ chunks.
    std::mt19937_64 rng(0xC0DECAFE);
    std::vector<std::pair<std::int64_t, double>> expected;
    for (int i = 0; i < 1500; ++i) {
        const auto t = static_cast<std::int64_t>(i) * 1000;
        const auto v = std::bit_cast<double>(rng());
        expected.emplace_back(t, v);
        ASSERT_TRUE(head.append(lbl, t, v).has_value());
    }

    auto dir = *b::Block::create_from_head(tmp.path() / "blocks", head);
    auto blk = *b::Block::open(dir);

    EXPECT_EQ(blk.meta().stats.num_series, 1u);
    EXPECT_GT(blk.meta().stats.num_chunks, 1u);
    EXPECT_EQ(blk.meta().stats.num_samples, expected.size());

    auto results = blk.query("__name__", "metric");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);

    // Concatenate every chunk's samples in chunk-meta order.
    std::vector<std::pair<std::int64_t, double>> recovered;
    for (const auto& chunk : (*results)[0].chunks) {
        auto it = chunk.iterator();
        while (it.next()) recovered.emplace_back(it.t(), it.v());
        EXPECT_FALSE(it.error().has_value());
    }
    ASSERT_EQ(recovered.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(recovered[i].first, expected[i].first) << "i=" << i;
        EXPECT_EQ(std::bit_cast<std::uint64_t>(recovered[i].second),
                  std::bit_cast<std::uint64_t>(expected[i].second)) << "i=" << i;
    }
}

TEST(CreateFromHead, BlockMetaTimeRangeMatchesAppendRange) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    m::Labels lbl{{"foo", "bar"}};
    ASSERT_TRUE(head.append(lbl, 42, 1.0).has_value());
    ASSERT_TRUE(head.append(lbl, 9999, 2.0).has_value());

    auto dir = *b::Block::create_from_head(tmp.path() / "blocks", head);
    auto blk = *b::Block::open(dir);
    EXPECT_EQ(blk.meta().min_time, 42);
    EXPECT_EQ(blk.meta().max_time, 9999);
}

TEST(CreateFromHead, HeadStateIsNotMutated) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    m::Labels lbl{{"k", "v"}};
    ASSERT_TRUE(head.append(lbl, 100, 1.0).has_value());

    const auto series_before  = head.series().size();
    const auto samples_before = head.series().get(1)->num_samples();

    auto dir = *b::Block::create_from_head(tmp.path() / "blocks", head);
    EXPECT_TRUE(std::filesystem::exists(dir));

    EXPECT_EQ(head.series().size(),               series_before);
    EXPECT_EQ(head.series().get(1)->num_samples(), samples_before);

    // Head still works after the flush — append another sample, no error.
    ASSERT_TRUE(head.append(lbl, 200, 2.0).has_value());
    EXPECT_EQ(head.series().get(1)->num_samples(), samples_before + 1);
}

TEST(CreateFromHead, MultipleBlocksFromSameHeadHaveDistinctUlids) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    m::Labels lbl{{"k", "v"}};
    ASSERT_TRUE(head.append(lbl, 100, 1.0).has_value());

    auto d1 = *b::Block::create_from_head(tmp.path() / "blocks", head);
    // sleep_for would be more reliable, but two consecutive ULIDs almost
    // always differ even at the same millisecond thanks to the random
    // tail. Just check they're distinct.
    auto d2 = *b::Block::create_from_head(tmp.path() / "blocks", head);
    EXPECT_NE(d1.filename().string(), d2.filename().string());

    // Both blocks must open cleanly and report the same data.
    auto b1 = *b::Block::open(d1);
    auto b2 = *b::Block::open(d2);
    EXPECT_EQ(b1.meta().stats.num_series, 1u);
    EXPECT_EQ(b2.meta().stats.num_series, 1u);
}
