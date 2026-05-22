#include "merlion_tsdb/block/block.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/model/labels.hpp"

namespace b = merlion_tsdb::block;
namespace c = merlion_tsdb::chunkenc;
namespace m = merlion_tsdb::model;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "merlion_compact_%08x_%08x", rd(), rd());
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

// Build a chunk from explicit samples.
b::Block::ChunkInput make_chunk(
    std::initializer_list<std::pair<std::int64_t, double>> samples) {
    c::XORChunk ch;
    auto app = ch.appender();
    EXPECT_TRUE(app.has_value());
    std::int64_t mn = std::numeric_limits<std::int64_t>::max();
    std::int64_t mx = std::numeric_limits<std::int64_t>::min();
    for (auto [t, v] : samples) {
        EXPECT_TRUE(app->append(t, v));
        mn = std::min(mn, t);
        mx = std::max(mx, t);
    }
    b::Block::ChunkInput ci;
    ci.bytes.assign(ch.bytes().begin(), ch.bytes().end());
    ci.min_time = mn;
    ci.max_time = mx;
    return ci;
}

}  // namespace

TEST(Compact, EmptyInputRejected) {
    TempDir tmp;
    auto r = b::Block::compact(tmp.path(), {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(Compact, TwoNonOverlappingBlocksMerge) {
    TempDir tmp;
    // Block A — single series with t=[0..200].
    std::vector<b::Block::SeriesInput> a;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"__name__", "metric"}};
        s.chunks.push_back(make_chunk({{0, 1.0}, {100, 2.0}, {200, 3.0}}));
        a.push_back(std::move(s));
    }
    auto dir_a = *b::Block::create_from_series(tmp.path(), a);

    // Block B — same series with t=[300..500].
    std::vector<b::Block::SeriesInput> b_input;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"__name__", "metric"}};
        s.chunks.push_back(make_chunk({{300, 4.0}, {400, 5.0}, {500, 6.0}}));
        b_input.push_back(std::move(s));
    }
    auto dir_b = *b::Block::create_from_series(tmp.path(), b_input);

    const std::array<std::filesystem::path, 2> inputs{dir_a, dir_b};
    auto dir_c = *b::Block::compact(tmp.path() / "compacted", inputs);

    auto blk = *b::Block::open(dir_c);
    // 1 merged series, 2 chunks (one per input).
    EXPECT_EQ(blk.meta().stats.num_series, 1u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 2u);
    EXPECT_EQ(blk.meta().stats.num_samples, 6u);
    EXPECT_EQ(blk.meta().min_time, 0);
    EXPECT_EQ(blk.meta().max_time, 500);
    EXPECT_EQ(blk.meta().compaction.level, 2);
    EXPECT_EQ(blk.meta().compaction.sources.size(), 2u);

    auto results = blk.query("__name__", "metric");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    const auto& r = (*results)[0];

    // Walk every chunk in order; concatenate samples.
    std::vector<std::pair<std::int64_t, double>> got;
    for (const auto& chunk : r.chunks) {
        auto it = chunk.iterator();
        while (it.next()) got.emplace_back(it.t(), it.v());
    }
    ASSERT_EQ(got.size(), 6u);
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].first,  100 * static_cast<std::int64_t>(i));
        EXPECT_DOUBLE_EQ(got[i].second, static_cast<double>(i + 1));
    }
}

TEST(Compact, DisjointSeriesAreAllPreserved) {
    TempDir tmp;
    // Block A — series {job=api}.
    std::vector<b::Block::SeriesInput> a;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"job", "api"}};
        s.chunks.push_back(make_chunk({{0, 1.0}}));
        a.push_back(std::move(s));
    }
    auto dir_a = *b::Block::create_from_series(tmp.path(), a);

    // Block B — series {job=web}.
    std::vector<b::Block::SeriesInput> b_input;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"job", "web"}};
        s.chunks.push_back(make_chunk({{0, 2.0}}));
        b_input.push_back(std::move(s));
    }
    auto dir_b = *b::Block::create_from_series(tmp.path(), b_input);

    const std::array<std::filesystem::path, 2> inputs{dir_a, dir_b};
    auto dir_c = *b::Block::compact(tmp.path() / "compacted", inputs);

    auto blk = *b::Block::open(dir_c);
    EXPECT_EQ(blk.meta().stats.num_series, 2u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 2u);

    auto api = blk.query("job", "api");
    ASSERT_TRUE(api.has_value());
    EXPECT_EQ(api->size(), 1u);
    auto web = blk.query("job", "web");
    ASSERT_TRUE(web.has_value());
    EXPECT_EQ(web->size(), 1u);
}

TEST(Compact, LevelPromotionAndSourceAggregation) {
    TempDir tmp;
    std::vector<b::Block::SeriesInput> input;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        s.chunks.push_back(make_chunk({{0, 1.0}}));
        input.push_back(std::move(s));
    }
    auto d1 = *b::Block::create_from_series(tmp.path(), input);
    auto d2 = *b::Block::create_from_series(tmp.path(), input);
    auto d3 = *b::Block::create_from_series(tmp.path(), input);

    const std::array<std::filesystem::path, 3> inputs{d1, d2, d3};
    auto dir_out = *b::Block::compact(tmp.path() / "compacted", inputs);

    auto blk = *b::Block::open(dir_out);
    // All three inputs are level=1, so output is level=2.
    EXPECT_EQ(blk.meta().compaction.level, 2);
    // Sources: union of three distinct input ULIDs.
    EXPECT_EQ(blk.meta().compaction.sources.size(), 3u);
    // Sources are deduplicated + sorted (std::set seeds the writer).
    auto srcs = blk.meta().compaction.sources;
    auto sorted = srcs;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(srcs, sorted);
}

TEST(Compact, RepeatedCompactionAccumulatesSources) {
    TempDir tmp;
    std::vector<b::Block::SeriesInput> input;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"a", "b"}};
        s.chunks.push_back(make_chunk({{0, 1.0}}));
        input.push_back(std::move(s));
    }
    // Round 1: compact 2 fresh blocks → level=2, sources=[ulid1, ulid2].
    auto d1 = *b::Block::create_from_series(tmp.path(), input);
    auto d2 = *b::Block::create_from_series(tmp.path(), input);
    const std::array<std::filesystem::path, 2> r1{d1, d2};
    auto dlvl2 = *b::Block::compact(tmp.path() / "lvl2", r1);
    EXPECT_EQ(b::Block::open(dlvl2)->meta().compaction.level, 2);

    // Round 2: compact the level-2 block + a fresh level-1 block →
    // level=3, sources={ulid1, ulid2, ulid3}.
    auto d3 = *b::Block::create_from_series(tmp.path(), input);
    const std::array<std::filesystem::path, 2> r2{dlvl2, d3};
    auto dlvl3 = *b::Block::compact(tmp.path() / "lvl3", r2);
    auto blk = *b::Block::open(dlvl3);
    EXPECT_EQ(blk.meta().compaction.level, 3);
    EXPECT_EQ(blk.meta().compaction.sources.size(), 3u);
}

TEST(Compact, ChunksAreOrderedByMinTime) {
    TempDir tmp;
    // Build blocks in reverse time order to verify sort-by-min_time.
    auto block_at = [&](std::int64_t base) {
        std::vector<b::Block::SeriesInput> in;
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        s.chunks.push_back(make_chunk({{base, 1.0}, {base + 100, 2.0}}));
        in.push_back(std::move(s));
        return *b::Block::create_from_series(tmp.path(), in);
    };

    // Build 3 blocks at base=200, 0, 100 (intentionally out-of-order).
    auto d_late   = block_at(200);
    auto d_early  = block_at(0);
    auto d_middle = block_at(100);
    const std::array<std::filesystem::path, 3> inputs{d_late, d_early, d_middle};

    auto dir_out = *b::Block::compact(tmp.path() / "compacted", inputs);
    auto blk = *b::Block::open(dir_out);

    auto results = blk.query("k", "v");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    const auto& r = (*results)[0];
    ASSERT_EQ(r.chunk_metas.size(), 3u);
    // Despite the unsorted input order, chunk_metas in the output block
    // must be in ascending min_time order.
    EXPECT_EQ(r.chunk_metas[0].min_time, 0);
    EXPECT_EQ(r.chunk_metas[1].min_time, 100);
    EXPECT_EQ(r.chunk_metas[2].min_time, 200);
}

TEST(Compact, AggregateQueryRecoversAllInputSeries) {
    TempDir tmp;
    // Build 3 blocks, each with 5 distinct series.
    auto make_block = [&](int batch_id) {
        std::vector<b::Block::SeriesInput> in;
        for (int i = 0; i < 5; ++i) {
            b::Block::SeriesInput s;
            s.labels = m::Labels{
                {"__name__", "metric"},
                {"batch",    std::to_string(batch_id)},
                {"id",       std::to_string(i)},
            };
            s.chunks.push_back(make_chunk({{batch_id * 1000, 1.0}}));
            in.push_back(std::move(s));
        }
        return *b::Block::create_from_series(tmp.path(), in);
    };
    auto d1 = make_block(0);
    auto d2 = make_block(1);
    auto d3 = make_block(2);

    const std::array<std::filesystem::path, 3> inputs{d1, d2, d3};
    auto dir_out = *b::Block::compact(tmp.path() / "compacted", inputs);
    auto blk = *b::Block::open(dir_out);

    EXPECT_EQ(blk.meta().stats.num_series, 15u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 15u);
    EXPECT_EQ(blk.meta().stats.num_samples, 15u);

    // Walk the postings table; each distinct label set should appear once.
    std::set<std::string> seen;
    for (const auto& entry : blk.index().postings_table().entries()) {
        if (entry.name.empty() && entry.value.empty()) continue;
        auto results = blk.query(entry.name, entry.value);
        ASSERT_TRUE(results.has_value());
        for (const auto& r : *results) {
            std::string key;
            for (const auto& l : r.labels.entries()) {
                if (!key.empty()) key += ",";
                key += l.name + "=" + l.value;
            }
            seen.insert(std::move(key));
        }
    }
    EXPECT_EQ(seen.size(), 15u);
}

TEST(Compact, InputDirectoriesAreUntouched) {
    TempDir tmp;
    std::vector<b::Block::SeriesInput> in;
    b::Block::SeriesInput s;
    s.labels = m::Labels{{"k", "v"}};
    s.chunks.push_back(make_chunk({{0, 1.0}}));
    in.push_back(std::move(s));

    auto d1 = *b::Block::create_from_series(tmp.path(), in);
    auto d2 = *b::Block::create_from_series(tmp.path(), in);
    const std::array<std::filesystem::path, 2> inputs{d1, d2};
    auto dir_out = *b::Block::compact(tmp.path() / "out", inputs);

    EXPECT_TRUE(std::filesystem::exists(d1));
    EXPECT_TRUE(std::filesystem::exists(d2));
    EXPECT_TRUE(std::filesystem::exists(dir_out));
    // Inputs are still queryable individually.
    auto b1 = *b::Block::open(d1);
    EXPECT_EQ(b1.meta().stats.num_series, 1u);
}
