#include "merlion_tsdb/block/block.hpp"
#include "merlion_tsdb/model/matcher.hpp"

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

#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/model/labels.hpp"

namespace b = merlion_tsdb::block;
namespace c = merlion_tsdb::chunkenc;
namespace m = merlion_tsdb::model;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "merlion_select_%08x_%08x", rd(), rd());
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

// Fixture: a small block with five series across two metric names and
// two jobs, plus one series that lacks the `job` label so we can
// exercise empty-value semantics.
struct Fixture {
    TempDir tmp;
    std::filesystem::path block_dir;

    Fixture() {
        std::vector<b::Block::SeriesInput> in;
        // s1: up{job="api"}              chunks at t∈[0,30],[100,200]
        in.push_back(series(m::Labels{{"__name__", "up"}, {"job", "api"}},
                            {make_chunk({{0, 1.0}, {30, 1.0}}),
                             make_chunk({{100, 1.0}, {200, 1.0}})}));
        // s2: up{job="web"}              chunk at t∈[0,200]
        in.push_back(series(m::Labels{{"__name__", "up"}, {"job", "web"}},
                            {make_chunk({{0, 0.0}, {200, 1.0}})}));
        // s3: requests{job="api"}        chunk at t∈[0,100]
        in.push_back(series(m::Labels{{"__name__", "requests"}, {"job", "api"}},
                            {make_chunk({{0, 5.0}, {100, 7.0}})}));
        // s4: requests{job="web"}        chunk at t∈[300,400]
        in.push_back(series(m::Labels{{"__name__", "requests"}, {"job", "web"}},
                            {make_chunk({{300, 1.0}, {400, 2.0}})}));
        // s5: requests{}    (no job)     chunk at t∈[0,100]
        in.push_back(series(m::Labels{{"__name__", "requests"}},
                            {make_chunk({{0, 11.0}, {100, 13.0}})}));

        auto d = b::Block::create_from_series(tmp.path(), in);
        EXPECT_TRUE(d.has_value());
        block_dir = *d;
    }
};

std::vector<m::Labels> labels_of(const std::vector<b::Block::QueryResult>& rs) {
    std::vector<m::Labels> out;
    out.reserve(rs.size());
    for (const auto& r : rs) out.push_back(r.labels);
    return out;
}

constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

}  // namespace

TEST(Select, RejectsEmptyMatcherSet) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    auto r = blk.select({}, k_min, k_max);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(Select, SingleEqMatchesExactSeries) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    std::array ms{m::Matcher::equal("__name__", "up")};
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    auto lbls = labels_of(*r);
    // Sorted by ascending series id (i.e. input order).
    EXPECT_EQ(lbls[0], (m::Labels{{"__name__", "up"}, {"job", "api"}}));
    EXPECT_EQ(lbls[1], (m::Labels{{"__name__", "up"}, {"job", "web"}}));
}

TEST(Select, MultipleEqMatchersAreAnded) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", "api"),
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "requests"}, {"job", "api"}}));
}

TEST(Select, NeqExcludesMatchingValue) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        m::Matcher::not_equal("job", "web"),
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "up"}, {"job", "api"}}));
}

TEST(Select, EqEmptyValueMatchesSeriesWithoutLabel) {
    // Eq("job", "") → series where `job` is absent.
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", ""),
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels, (m::Labels{{"__name__", "requests"}}));
}

TEST(Select, NeqEmptyValueMatchesSeriesWithLabel) {
    // Neq("job", "") → series where `job` is present (non-empty).
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::not_equal("job", ""),
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    // s3 and s4 — both have `job`.
}

TEST(Select, RegexUnionsMatchingValues) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    auto re = m::Matcher::regex("job", "api|web");
    ASSERT_TRUE(re.has_value());
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        *re,
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 2u);
}

TEST(Select, RegexIsAnchored) {
    // Pattern "api" must NOT match "apixyz" since the implementation
    // anchors with ^(...)$.
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    auto re = m::Matcher::regex("job", "ap");
    ASSERT_TRUE(re.has_value());
    auto r = blk.select(std::array{*re}, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 0u);  // "ap" anchored never matches "api" or "web"
}

TEST(Select, NregexExcludesMatchingValues) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    auto nre = m::Matcher::not_regex("job", "web");
    ASSERT_TRUE(nre.has_value());
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        *nre,
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "up"}, {"job", "api"}}));
}

TEST(Select, NregexIncludesSeriesWithoutLabelWhenPatternMatchesEmpty) {
    // Nre("job", "web") — pattern "web" does NOT match "", so series
    // without `job` ARE included (their conceptual value "" satisfies
    // !regex("web")). This is upstream PromQL semantics.
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    auto nre = m::Matcher::not_regex("job", "web");
    ASSERT_TRUE(nre.has_value());
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        *nre,
    };
    auto r = blk.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    // s3 (job=api) survives, s4 (job=web) excluded, s5 (no job) included.
    ASSERT_EQ(r->size(), 2u);
}

TEST(Select, TimeRangeFiltersChunks) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    // up{job="api"} has chunks [0,30] and [100,200]. Query [50, 90] —
    // neither chunk overlaps, so series is dropped entirely.
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        m::Matcher::equal("job", "api"),
    };
    auto r = blk.select(ms, 50, 90);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 0u);
}

TEST(Select, TimeRangePartiallyOverlappingKeepsOnlyOverlappingChunks) {
    Fixture f;
    auto blk = *b::Block::open(f.block_dir);
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        m::Matcher::equal("job", "api"),
    };
    // Range [25, 150] hits BOTH chunks: [0,30] overlaps via 25-30,
    // [100,200] overlaps via 100-150.
    auto r = blk.select(ms, 25, 150);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].chunks.size(), 2u);

    // Range [40, 90] hits NEITHER.
    auto r2 = blk.select(ms, 40, 90);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->size(), 0u);

    // Range [40, 150] hits only [100,200].
    auto r3 = blk.select(ms, 40, 150);
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3->size(), 1u);
    EXPECT_EQ((*r3)[0].chunks.size(), 1u);
}

TEST(Select, RegexCompilationFailureReportsError) {
    auto bad = m::Matcher::regex("foo", "([unterminated");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), std::make_error_code(std::errc::invalid_argument));
}
