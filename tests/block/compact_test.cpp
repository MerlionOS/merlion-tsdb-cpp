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
    // 1 merged series. Vertical-merge re-encodes all 6 samples through a
    // throwaway MemSeries; they fit comfortably under the 1 KiB soft
    // cap, so the output is a single chunk (vs. the 2 verbatim chunks
    // the old concat-only path produced).
    EXPECT_EQ(blk.meta().stats.num_series, 1u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 1u);
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

TEST(Compact, SamplesAreOrderedRegardlessOfInputBlockOrder) {
    TempDir tmp;
    // Each block has samples at (base, 1.0) and (base+100, 2.0). With
    // base ∈ {200, 0, 100} the time ranges overlap at t=100 and t=200,
    // so vertical-merge collapses 6 raw samples into 4 unique
    // timestamps.
    auto block_at = [&](std::int64_t base) {
        std::vector<b::Block::SeriesInput> in;
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        s.chunks.push_back(make_chunk({{base, 1.0}, {base + 100, 2.0}}));
        in.push_back(std::move(s));
        return *b::Block::create_from_series(tmp.path(), in);
    };

    // Input order: [d_late, d_early, d_middle]. Last-wins means
    // d_middle's values prevail on overlapping timestamps (t=100 and
    // t=200).
    auto d_late   = block_at(200);  // (200, 1.0), (300, 2.0)
    auto d_early  = block_at(0);    // (0,   1.0), (100, 2.0)
    auto d_middle = block_at(100);  // (100, 1.0), (200, 2.0)
    const std::array<std::filesystem::path, 3> inputs{d_late, d_early, d_middle};

    auto dir_out = *b::Block::compact(tmp.path() / "compacted", inputs);
    auto blk = *b::Block::open(dir_out);

    auto results = blk.query("k", "v");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    const auto& r = (*results)[0];

    std::vector<std::pair<std::int64_t, double>> got;
    for (const auto& chunk : r.chunks) {
        auto it = chunk.iterator();
        while (it.next()) got.emplace_back(it.t(), it.v());
    }
    ASSERT_EQ(got.size(), 4u);
    // Strict ascending timestamps regardless of input block order.
    EXPECT_EQ(got[0].first, 0);
    EXPECT_EQ(got[1].first, 100);
    EXPECT_EQ(got[2].first, 200);
    EXPECT_EQ(got[3].first, 300);
    // Values follow last-input-wins:
    //   t=0   only in d_early                              → 1.0
    //   t=100 in d_early (2.0) + d_middle (1.0), last wins → 1.0
    //   t=200 in d_late  (1.0) + d_middle (2.0), last wins → 2.0
    //   t=300 only in d_late                               → 2.0
    EXPECT_DOUBLE_EQ(got[0].second, 1.0);
    EXPECT_DOUBLE_EQ(got[1].second, 1.0);
    EXPECT_DOUBLE_EQ(got[2].second, 2.0);
    EXPECT_DOUBLE_EQ(got[3].second, 2.0);

    EXPECT_EQ(blk.meta().min_time, 0);
    EXPECT_EQ(blk.meta().max_time, 300);
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

TEST(Compact, OverlappingTimestampsAreDeduplicated) {
    TempDir tmp;
    // Block A — series with samples at t=100, 200, 300.
    std::vector<b::Block::SeriesInput> a;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        s.chunks.push_back(make_chunk({{100, 1.0}, {200, 2.0}, {300, 3.0}}));
        a.push_back(std::move(s));
    }
    auto dir_a = *b::Block::create_from_series(tmp.path(), a);

    // Block B — same series, samples at t=200, 300, 400. Two t-values
    // (200, 300) overlap with block A.
    std::vector<b::Block::SeriesInput> b_input;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        s.chunks.push_back(make_chunk({{200, 20.0}, {300, 30.0}, {400, 40.0}}));
        b_input.push_back(std::move(s));
    }
    auto dir_b = *b::Block::create_from_series(tmp.path(), b_input);

    const std::array<std::filesystem::path, 2> inputs{dir_a, dir_b};
    auto dir_c = *b::Block::compact(tmp.path() / "compacted", inputs);
    auto blk = *b::Block::open(dir_c);

    // Total unique timestamps = 4 (100, 200, 300, 400), not 6.
    EXPECT_EQ(blk.meta().stats.num_samples, 4u);

    auto results = blk.query("k", "v");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    std::vector<std::pair<std::int64_t, double>> got;
    for (const auto& chunk : (*results)[0].chunks) {
        auto it = chunk.iterator();
        while (it.next()) got.emplace_back(it.t(), it.v());
    }
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0].first,  100);
    EXPECT_DOUBLE_EQ(got[0].second, 1.0);   // only in A
    EXPECT_EQ(got[1].first,  200);
    EXPECT_DOUBLE_EQ(got[1].second, 20.0);  // overlap: B's value wins (later input)
    EXPECT_EQ(got[2].first,  300);
    EXPECT_DOUBLE_EQ(got[2].second, 30.0);  // overlap: B's value wins
    EXPECT_EQ(got[3].first,  400);
    EXPECT_DOUBLE_EQ(got[3].second, 40.0);  // only in B
}

TEST(Compact, LastInputWinsOnDuplicateTimestamp) {
    // Three blocks all carry the same (series, t=100) sample with
    // different values. The merged output must reflect the LAST input's
    // value (input order = block-list order).
    TempDir tmp;
    auto make_block = [&](double v) {
        std::vector<b::Block::SeriesInput> in;
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        s.chunks.push_back(make_chunk({{100, v}}));
        in.push_back(std::move(s));
        return *b::Block::create_from_series(tmp.path(), in);
    };
    auto d1 = make_block(1.0);
    auto d2 = make_block(2.0);
    auto d3 = make_block(3.0);
    const std::array<std::filesystem::path, 3> inputs{d1, d2, d3};

    auto dir_out = *b::Block::compact(tmp.path() / "compacted", inputs);
    auto blk = *b::Block::open(dir_out);
    EXPECT_EQ(blk.meta().stats.num_samples, 1u);

    auto results = blk.query("k", "v");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    auto it = (*results)[0].chunks[0].iterator();
    ASSERT_TRUE(it.next());
    EXPECT_EQ(it.t(), 100);
    EXPECT_DOUBLE_EQ(it.v(), 3.0);  // d3 was last in the input span
    EXPECT_FALSE(it.next());
}

TEST(Compact, LargeMergeProducesMultipleChunks) {
    // Stress: enough samples that the soft cap forces multiple output
    // chunks. The vertical-merge code path delegates to MemSeries which
    // already does this correctly for the head→block flush — this test
    // confirms the same machinery works under compaction.
    TempDir tmp;
    // Build two blocks each holding 1500 random samples in disjoint
    // time ranges. Random values defeat XOR's same-delta compression
    // so chunks stay near the 1 KiB soft cap.
    std::mt19937_64 rng(0xCAFEBABE);
    auto make_block = [&](std::int64_t base) {
        std::vector<b::Block::SeriesInput> in;
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"k", "v"}};
        for (int i = 0; i < 1500; ++i) {
            s.chunks.push_back(make_chunk({{
                base + static_cast<std::int64_t>(i),
                std::bit_cast<double>(rng()),
            }}));
        }
        in.push_back(std::move(s));
        return *b::Block::create_from_series(tmp.path(), in);
    };
    auto d1 = make_block(0);
    auto d2 = make_block(2'000);
    const std::array<std::filesystem::path, 2> inputs{d1, d2};

    auto dir_out = *b::Block::compact(tmp.path() / "compacted", inputs);
    auto blk = *b::Block::open(dir_out);
    EXPECT_EQ(blk.meta().stats.num_samples, 3000u);
    EXPECT_GT(blk.meta().stats.num_chunks, 1u);  // multiple chunks due to soft cap

    // Aggregate all decoded samples; every input timestamp must appear
    // exactly once.
    auto results = *blk.query("k", "v");
    ASSERT_EQ(results.size(), 1u);
    std::vector<std::int64_t> ts;
    for (const auto& chunk : results[0].chunks) {
        auto it = chunk.iterator();
        while (it.next()) ts.push_back(it.t());
    }
    ASSERT_EQ(ts.size(), 3000u);
    for (std::size_t i = 1; i < ts.size(); ++i) {
        EXPECT_LT(ts[i - 1], ts[i]);  // strictly ascending
    }
}
