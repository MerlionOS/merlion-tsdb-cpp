#include "merlion_tsdb/querier/querier.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/block/block.hpp"
#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/head/head.hpp"
#include "merlion_tsdb/model/labels.hpp"
#include "merlion_tsdb/model/matcher.hpp"

namespace b = merlion_tsdb::block;
namespace c = merlion_tsdb::chunkenc;
namespace h = merlion_tsdb::head;
namespace m = merlion_tsdb::model;
namespace q = merlion_tsdb::querier;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "merlion_querier_%08x_%08x", rd(), rd());
        path_ = std::filesystem::temp_directory_path() / buf;
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

b::Block::ChunkInput make_chunk(
    std::initializer_list<std::pair<std::int64_t, double>> samples) {
    c::XORChunk chunk;
    auto app = chunk.appender();
    EXPECT_TRUE(app.has_value());
    std::int64_t min_t = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_t = std::numeric_limits<std::int64_t>::min();
    for (auto [t, v] : samples) {
        EXPECT_TRUE(app->append(t, v));
        if (t < min_t) min_t = t;
        if (t > max_t) max_t = t;
    }
    b::Block::ChunkInput ci;
    ci.bytes.assign(chunk.bytes().begin(), chunk.bytes().end());
    ci.min_time = min_t;
    ci.max_time = max_t;
    return ci;
}

b::Block::SeriesInput series(m::Labels lbls,
                             std::vector<b::Block::ChunkInput> chunks) {
    b::Block::SeriesInput s;
    s.labels = std::move(lbls);
    s.chunks = std::move(chunks);
    return s;
}

std::filesystem::path make_block(const std::filesystem::path& parent,
                                 std::vector<b::Block::SeriesInput> in) {
    auto d = b::Block::create_from_series(parent, in);
    EXPECT_TRUE(d.has_value());
    return *d;
}

std::vector<std::pair<std::int64_t, double>>
decode_all(const q::MergedSeries& s) {
    std::vector<std::pair<std::int64_t, double>> out;
    for (const auto& chunk : s.chunks) {
        auto it = chunk.iterator();
        while (it.next()) out.emplace_back(it.t(), it.v());
    }
    return out;
}

constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

}  // namespace

TEST(Querier, RejectsEmptyMatcherSet) {
    TempDir tmp;
    auto d = make_block(tmp.path(),
        {series(m::Labels{{"__name__", "up"}}, {make_chunk({{0, 1.0}})})});
    auto blk = *b::Block::open(d);
    const b::Block* ptrs[] = {&blk};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    auto r = qr.select({}, k_min, k_max);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(Querier, SingleBlockMatchesBlockSelect) {
    TempDir tmp;
    auto d = make_block(tmp.path(), {
        series(m::Labels{{"__name__", "up"}, {"job", "api"}},
               {make_chunk({{0, 1.0}, {30, 1.0}})}),
        series(m::Labels{{"__name__", "up"}, {"job", "web"}},
               {make_chunk({{0, 0.0}, {30, 0.0}})}),
    });
    auto blk = *b::Block::open(d);
    const b::Block* ptrs[] = {&blk};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("__name__", "up")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 2u);
}

TEST(Querier, DisjointTimeRangeBlocksMergeSameSeries) {
    // Two blocks, same series, chunks in disjoint time windows. The
    // querier should return one MergedSeries with chunks ordered by
    // min_time across both blocks.
    TempDir tmp;
    auto d1 = make_block(tmp.path() / "b1", {
        series(m::Labels{{"__name__", "up"}, {"job", "api"}},
               {make_chunk({{0, 1.0}, {100, 1.0}})}),
    });
    auto d2 = make_block(tmp.path() / "b2", {
        series(m::Labels{{"__name__", "up"}, {"job", "api"}},
               {make_chunk({{200, 1.0}, {300, 1.0}})}),
    });
    auto blk1 = *b::Block::open(d1);
    auto blk2 = *b::Block::open(d2);
    const b::Block* ptrs[] = {&blk2, &blk1};  // intentionally reversed order
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("__name__", "up")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    const auto& s = (*r)[0];
    EXPECT_EQ(s.labels, (m::Labels{{"__name__", "up"}, {"job", "api"}}));
    ASSERT_EQ(s.chunks.size(), 2u);
    // Chunks must be ordered by min_time regardless of input block order.
    EXPECT_EQ(s.chunk_metas[0].min_time, 0);
    EXPECT_EQ(s.chunk_metas[1].min_time, 200);

    auto samples = decode_all(s);
    ASSERT_EQ(samples.size(), 4u);
    EXPECT_EQ(samples[0].first, 0);
    EXPECT_EQ(samples[3].first, 300);
}

TEST(Querier, DisjointSeriesAcrossBlocksUnion) {
    TempDir tmp;
    auto d1 = make_block(tmp.path() / "b1", {
        series(m::Labels{{"__name__", "up"}, {"job", "api"}},
               {make_chunk({{0, 1.0}, {100, 1.0}})}),
    });
    auto d2 = make_block(tmp.path() / "b2", {
        series(m::Labels{{"__name__", "up"}, {"job", "web"}},
               {make_chunk({{0, 0.0}, {100, 0.0}})}),
    });
    auto blk1 = *b::Block::open(d1);
    auto blk2 = *b::Block::open(d2);
    const b::Block* ptrs[] = {&blk1, &blk2};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("__name__", "up")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 2u);
    // Both label sets appear, no dedup possible across distinct labels.
    bool saw_api = false, saw_web = false;
    for (const auto& s : *r) {
        if (s.labels == m::Labels{{"__name__", "up"}, {"job", "api"}}) saw_api = true;
        if (s.labels == m::Labels{{"__name__", "up"}, {"job", "web"}}) saw_web = true;
    }
    EXPECT_TRUE(saw_api);
    EXPECT_TRUE(saw_web);
}

TEST(Querier, TimeRangeSkipsBlocksOutsideRange) {
    // Block 1 covers [0, 100], block 2 covers [200, 300]. Query
    // [150, 250] should only see block 2's chunks for the matching
    // series.
    TempDir tmp;
    auto d1 = make_block(tmp.path() / "b1", {
        series(m::Labels{{"__name__", "up"}},
               {make_chunk({{0, 1.0}, {100, 1.0}})}),
    });
    auto d2 = make_block(tmp.path() / "b2", {
        series(m::Labels{{"__name__", "up"}},
               {make_chunk({{200, 1.0}, {300, 1.0}})}),
    });
    auto blk1 = *b::Block::open(d1);
    auto blk2 = *b::Block::open(d2);
    const b::Block* ptrs[] = {&blk1, &blk2};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("__name__", "up")};
    auto r = qr.select(ms, 150, 250);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].chunks.size(), 1u);
    EXPECT_EQ((*r)[0].chunk_metas[0].min_time, 200);
}

TEST(Querier, ChunksMergedFromMultipleBlocksAreSorted) {
    // Three blocks, all containing one series, with chunks at out-of-
    // order min_times. The output must be globally sorted by min_time.
    TempDir tmp;
    auto d_mid  = make_block(tmp.path() / "b_mid",
        {series(m::Labels{{"k", "v"}}, {make_chunk({{500, 1.0}, {600, 2.0}})})});
    auto d_late = make_block(tmp.path() / "b_late",
        {series(m::Labels{{"k", "v"}}, {make_chunk({{900, 1.0}, {1000, 2.0}})})});
    auto d_early = make_block(tmp.path() / "b_early",
        {series(m::Labels{{"k", "v"}}, {make_chunk({{100, 1.0}, {200, 2.0}})})});

    auto bm  = *b::Block::open(d_mid);
    auto bl  = *b::Block::open(d_late);
    auto be  = *b::Block::open(d_early);
    const b::Block* ptrs[] = {&bm, &bl, &be};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    const auto& s = (*r)[0];
    ASSERT_EQ(s.chunk_metas.size(), 3u);
    EXPECT_EQ(s.chunk_metas[0].min_time, 100);
    EXPECT_EQ(s.chunk_metas[1].min_time, 500);
    EXPECT_EQ(s.chunk_metas[2].min_time, 900);
}

TEST(Querier, MatcherAppliedPerBlockNotJustGlobally) {
    // Both blocks contain `up{job=api}` and `up{job=web}`. The query
    // `job=web` should suppress the api series in BOTH blocks.
    TempDir tmp;
    auto d1 = make_block(tmp.path() / "b1", {
        series(m::Labels{{"__name__", "up"}, {"job", "api"}},
               {make_chunk({{0, 1.0}})}),
        series(m::Labels{{"__name__", "up"}, {"job", "web"}},
               {make_chunk({{0, 0.0}})}),
    });
    auto d2 = make_block(tmp.path() / "b2", {
        series(m::Labels{{"__name__", "up"}, {"job", "api"}},
               {make_chunk({{100, 1.0}})}),
        series(m::Labels{{"__name__", "up"}, {"job", "web"}},
               {make_chunk({{100, 0.0}})}),
    });
    auto blk1 = *b::Block::open(d1);
    auto blk2 = *b::Block::open(d2);
    const b::Block* ptrs[] = {&blk1, &blk2};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("job", "web")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "up"}, {"job", "web"}}));
    EXPECT_EQ((*r)[0].chunks.size(), 2u);  // one per block
}

TEST(Querier, EmptyBlockListReturnsEmptyResult) {
    q::Querier qr{std::span<const b::Block* const>{}};
    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

TEST(Querier, NullBlockPointersAreSkipped) {
    TempDir tmp;
    auto d = make_block(tmp.path(),
        {series(m::Labels{{"k", "v"}}, {make_chunk({{0, 1.0}})})});
    auto blk = *b::Block::open(d);
    const b::Block* ptrs[] = {nullptr, &blk, nullptr};
    q::Querier qr{std::span<const b::Block* const>{ptrs}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 1u);
}

// -----------------------------------------------------------------------
// §10: unified queries across Block + Head
// -----------------------------------------------------------------------

TEST(Querier, HeadOnlyConstructorRunsMatchersAgainstHead) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 10, 1.0).has_value());
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 20, 2.0).has_value());

    const h::Head* heads[] = {&head};
    q::Querier qr{std::span<const b::Block* const>{},
                  std::span<const h::Head* const>{heads}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels, (m::Labels{{"k", "v"}}));
    auto samples = decode_all((*r)[0]);
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].first, 10);
    EXPECT_EQ(samples[1].first, 20);
}

TEST(Querier, HeadAndBlockSameSeriesMergeChunksInTimeOrder) {
    // Block holds the "old" chunk window [0, 100]; Head holds the
    // "recent" chunk at [200, 300]. The unified querier must return one
    // MergedSeries with chunks ordered by min_time across sources, NOT
    // by block-then-head insertion order.
    TempDir tmp;
    auto d = make_block(tmp.path() / "blk", {
        series(m::Labels{{"k", "v"}},
               {make_chunk({{0, 1.0}, {100, 2.0}})}),
    });
    auto blk = *b::Block::open(d);

    auto head = *h::Head::open(tmp.path() / "head");
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 200, 3.0).has_value());
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 300, 4.0).has_value());

    const b::Block* blocks[] = {&blk};
    const h::Head*  heads[]  = {&head};
    q::Querier qr{std::span<const b::Block* const>{blocks},
                  std::span<const h::Head* const>{heads}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    const auto& s = (*r)[0];
    ASSERT_EQ(s.chunk_metas.size(), 2u);
    EXPECT_EQ(s.chunk_metas[0].min_time, 0);    // block chunk first
    EXPECT_EQ(s.chunk_metas[1].min_time, 200);  // head chunk after

    auto samples = decode_all(s);
    ASSERT_EQ(samples.size(), 4u);
    EXPECT_EQ(samples.front().first, 0);
    EXPECT_EQ(samples.back().first, 300);
}

TEST(Querier, HeadChunkComesBeforeBlockChunkWhenTimesDictate) {
    // Head holds the *earlier* chunk; Block holds the later one. The
    // sort by min_time must promote the head chunk to position 0 even
    // though heads are processed after blocks internally.
    TempDir tmp;
    auto d = make_block(tmp.path() / "blk", {
        series(m::Labels{{"k", "v"}},
               {make_chunk({{500, 5.0}, {600, 6.0}})}),
    });
    auto blk = *b::Block::open(d);

    auto head = *h::Head::open(tmp.path() / "head");
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 10, 1.0).has_value());
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 20, 2.0).has_value());

    const b::Block* blocks[] = {&blk};
    const h::Head*  heads[]  = {&head};
    q::Querier qr{std::span<const b::Block* const>{blocks},
                  std::span<const h::Head* const>{heads}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    const auto& s = (*r)[0];
    ASSERT_EQ(s.chunk_metas.size(), 2u);
    EXPECT_EQ(s.chunk_metas[0].min_time, 10);   // head chunk first
    EXPECT_EQ(s.chunk_metas[1].min_time, 500);
}

TEST(Querier, HeadOnlySeriesUnionsWithBlockOnlySeries) {
    // Head holds `{k=head_only}` and block holds `{k=block_only}`. A
    // matcher that admits both should return two distinct MergedSeries.
    TempDir tmp;
    auto d = make_block(tmp.path() / "blk", {
        series(m::Labels{{"k", "block_only"}}, {make_chunk({{0, 1.0}})}),
    });
    auto blk = *b::Block::open(d);

    auto head = *h::Head::open(tmp.path() / "head");
    ASSERT_TRUE(head.append(m::Labels{{"k", "head_only"}}, 10, 2.0).has_value());

    const b::Block* blocks[] = {&blk};
    const h::Head*  heads[]  = {&head};
    q::Querier qr{std::span<const b::Block* const>{blocks},
                  std::span<const h::Head* const>{heads}};

    auto re = m::Matcher::regex("k", ".*_only");
    ASSERT_TRUE(re.has_value());
    auto r = qr.select(std::array{*re}, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 2u);
}

TEST(Querier, TimeRangeFiltersHeadAndBlockIndependently) {
    // Block chunk at [0, 100]; head chunk at [200, 300]. Query [250, 400]
    // must drop the block chunk and keep the head chunk.
    TempDir tmp;
    auto d = make_block(tmp.path() / "blk", {
        series(m::Labels{{"k", "v"}},
               {make_chunk({{0, 1.0}, {100, 2.0}})}),
    });
    auto blk = *b::Block::open(d);

    auto head = *h::Head::open(tmp.path() / "head");
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 200, 3.0).has_value());
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 300, 4.0).has_value());

    const b::Block* blocks[] = {&blk};
    const h::Head*  heads[]  = {&head};
    q::Querier qr{std::span<const b::Block* const>{blocks},
                  std::span<const h::Head* const>{heads}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, 250, 400);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    ASSERT_EQ((*r)[0].chunks.size(), 1u);
    EXPECT_EQ((*r)[0].chunk_metas[0].min_time, 200);  // only head chunk survives
}

TEST(Querier, NullHeadPointersAreSkipped) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path() / "head");
    ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 0, 1.0).has_value());

    const h::Head* heads[] = {nullptr, &head, nullptr};
    q::Querier qr{std::span<const b::Block* const>{},
                  std::span<const h::Head* const>{heads}};

    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 1u);
}

TEST(Querier, EmptyHeadAndBlockListsReturnEmptyResult) {
    q::Querier qr{std::span<const b::Block* const>{},
                  std::span<const h::Head* const>{}};
    std::array ms{m::Matcher::equal("k", "v")};
    auto r = qr.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}
