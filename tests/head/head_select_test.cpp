#include "merlion_tsdb/head/head.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/chunkenc/xor.hpp"
#include "merlion_tsdb/model/labels.hpp"
#include "merlion_tsdb/model/matcher.hpp"

namespace h = merlion_tsdb::head;
namespace c = merlion_tsdb::chunkenc;
namespace m = merlion_tsdb::model;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "merlion_head_select_%08x_%08x", rd(), rd());
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

// Same five-series fixture as block/select_test.cpp, but loaded into a
// Head instead of a Block. That lets the tests below assert the same
// matcher + time-range semantics across both layers.
struct Fixture {
    TempDir tmp;
    h::Head head;

    Fixture() : head(*h::Head::open(tmp.path())) {
        auto add = [&](const m::Labels& l, std::int64_t t, double v) {
            EXPECT_TRUE(head.append(l, t, v).has_value());
        };
        // s1: up{job="api"}  (single in-memory chunk — well under 1 KiB)
        add(m::Labels{{"__name__", "up"}, {"job", "api"}}, 0,   1.0);
        add(m::Labels{{"__name__", "up"}, {"job", "api"}}, 30,  1.0);
        add(m::Labels{{"__name__", "up"}, {"job", "api"}}, 100, 1.0);
        add(m::Labels{{"__name__", "up"}, {"job", "api"}}, 200, 1.0);
        // s2: up{job="web"}
        add(m::Labels{{"__name__", "up"}, {"job", "web"}}, 0,   0.0);
        add(m::Labels{{"__name__", "up"}, {"job", "web"}}, 200, 1.0);
        // s3: requests{job="api"}
        add(m::Labels{{"__name__", "requests"}, {"job", "api"}}, 0,   5.0);
        add(m::Labels{{"__name__", "requests"}, {"job", "api"}}, 100, 7.0);
        // s4: requests{job="web"}
        add(m::Labels{{"__name__", "requests"}, {"job", "web"}}, 300, 1.0);
        add(m::Labels{{"__name__", "requests"}, {"job", "web"}}, 400, 2.0);
        // s5: requests{}  (no job label)
        add(m::Labels{{"__name__", "requests"}}, 0,   11.0);
        add(m::Labels{{"__name__", "requests"}}, 100, 13.0);
    }
};

std::vector<m::Labels> labels_of(const std::vector<h::Head::QueryResult>& rs) {
    std::vector<m::Labels> out;
    out.reserve(rs.size());
    for (const auto& r : rs) out.push_back(r.labels);
    return out;
}

constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

bool contains(const std::vector<m::Labels>& lbls, const m::Labels& want) {
    for (const auto& l : lbls) if (l == want) return true;
    return false;
}

// Decode all samples from a series's chunks in order. Used by tests that
// verify the snapshot bytes round-trip correctly.
std::vector<std::pair<std::int64_t, double>>
all_samples(const h::Head::QueryResult& qr) {
    std::vector<std::pair<std::int64_t, double>> out;
    for (const auto& chunk : qr.chunks) {
        auto it = chunk.iterator();
        while (it.next()) out.emplace_back(it.t(), it.v());
    }
    return out;
}

}  // namespace

TEST(HeadSelect, RejectsEmptyMatcherSet) {
    Fixture f;
    auto r = f.head.select({}, k_min, k_max);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(HeadSelect, SingleEqMatchesExactSeries) {
    Fixture f;
    std::array ms{m::Matcher::equal("__name__", "up")};
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    auto lbls = labels_of(*r);
    EXPECT_TRUE(contains(lbls, m::Labels{{"__name__", "up"}, {"job", "api"}}));
    EXPECT_TRUE(contains(lbls, m::Labels{{"__name__", "up"}, {"job", "web"}}));
}

TEST(HeadSelect, MultipleEqMatchersAreAnded) {
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", "api"),
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "requests"}, {"job", "api"}}));
}

TEST(HeadSelect, NeqExcludesMatchingValue) {
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        m::Matcher::not_equal("job", "web"),
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "up"}, {"job", "api"}}));
}

TEST(HeadSelect, EqEmptyValueMatchesSeriesWithoutLabel) {
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", ""),
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels, (m::Labels{{"__name__", "requests"}}));
}

TEST(HeadSelect, NeqEmptyValueMatchesSeriesWithLabel) {
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::not_equal("job", ""),
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
}

TEST(HeadSelect, RegexUnionsMatchingValues) {
    Fixture f;
    auto re = m::Matcher::regex("job", "api|web");
    ASSERT_TRUE(re.has_value());
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        *re,
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 2u);
}

TEST(HeadSelect, RegexIsAnchored) {
    Fixture f;
    auto re = m::Matcher::regex("job", "ap");  // would partial-match "api"
    ASSERT_TRUE(re.has_value());
    auto r = f.head.select(std::array{*re}, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 0u);
}

TEST(HeadSelect, NregexExcludesMatchingValues) {
    Fixture f;
    auto nre = m::Matcher::not_regex("job", "web");
    ASSERT_TRUE(nre.has_value());
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        *nre,
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].labels,
              (m::Labels{{"__name__", "up"}, {"job", "api"}}));
}

TEST(HeadSelect, NregexIncludesSeriesWithoutLabelWhenPatternDoesNotMatchEmpty) {
    Fixture f;
    auto nre = m::Matcher::not_regex("job", "web");
    ASSERT_TRUE(nre.has_value());
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        *nre,
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    // s3 (job=api) and s5 (no job) survive; s4 (job=web) excluded.
    ASSERT_EQ(r->size(), 2u);
}

TEST(HeadSelect, TimeRangeDropsSeriesWithNoOverlappingChunks) {
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", "web"),
    };
    // s4 has samples at t=300,400 — query [0, 100] excludes them.
    auto r = f.head.select(ms, 0, 100);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 0u);
}

TEST(HeadSelect, TimeRangePartialOverlapKeepsSurvivingChunks) {
    // s1 (up{job=api}) lands all four samples in one head chunk (well
    // under the 1 KiB cap), so any range intersecting [0, 200] yields
    // exactly one chunk. Slightly different from block's two-chunk
    // fixture, but the semantics under test are still "overlap → keep,
    // disjoint → drop".
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "up"),
        m::Matcher::equal("job", "api"),
    };

    auto r_in = f.head.select(ms, 50, 150);
    ASSERT_TRUE(r_in.has_value());
    ASSERT_EQ(r_in->size(), 1u);
    EXPECT_EQ((*r_in)[0].chunks.size(), 1u);

    auto r_out = f.head.select(ms, 500, 600);
    ASSERT_TRUE(r_out.has_value());
    EXPECT_EQ(r_out->size(), 0u);
}

TEST(HeadSelect, ChunkMetaMinMaxTimeRecoveredFromSamples) {
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", "api"),
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    ASSERT_EQ((*r)[0].chunk_metas.size(), 1u);
    EXPECT_EQ((*r)[0].chunk_metas[0].min_time, 0);
    EXPECT_EQ((*r)[0].chunk_metas[0].max_time, 100);
    // In-memory chunks carry no on-disk ref.
    EXPECT_EQ((*r)[0].chunk_metas[0].ref, 0u);
}

TEST(HeadSelect, ChunkSnapshotIsIndependentOfFurtherAppends) {
    // Snapshot semantics: select() must hand back chunk bytes that
    // remain valid after additional appends to the underlying
    // MemSeries.
    Fixture f;
    std::array ms{
        m::Matcher::equal("__name__", "requests"),
        m::Matcher::equal("job", "api"),
    };
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    const auto before = all_samples((*r)[0]);

    // Mutate the head: same series gets a new, later sample.
    auto more = f.head.append(
        m::Labels{{"__name__", "requests"}, {"job", "api"}}, 9999, 99.0);
    ASSERT_TRUE(more.has_value());

    // Decode the snapshot bytes again — must equal the pre-append view.
    const auto after = all_samples((*r)[0]);
    EXPECT_EQ(before, after);
    EXPECT_EQ(before.size(), 2u);  // 2 samples (t=0, t=100), not 3
}

TEST(HeadSelect, MatchesNothingReturnsEmptyVector) {
    Fixture f;
    std::array ms{m::Matcher::equal("__name__", "no_such_metric")};
    auto r = f.head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

TEST(HeadSelect, EmptyHeadReturnsEmptyVector) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    std::array ms{m::Matcher::equal("__name__", "anything")};
    auto r = head.select(ms, k_min, k_max);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}
