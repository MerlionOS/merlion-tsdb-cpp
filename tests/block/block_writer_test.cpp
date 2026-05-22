#include "merlion_tsdb/block/block.hpp"
#include "merlion_tsdb/block/ulid.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>
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
        std::snprintf(buf, sizeof(buf), "merlion_blockw_%08x_%08x", rd(), rd());
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

// Builds an XOR chunk from a (t, v) sequence and returns it packaged as
// the SeriesInput::ChunkInput format the writer expects.
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
    b::Block::ChunkInput out;
    out.min_time = min_t;
    out.max_time = max_t;
    out.bytes.assign(chunk.bytes().begin(), chunk.bytes().end());
    return out;
}

}  // namespace

// --- ULID ------------------------------------------------------------------

TEST(Ulid, EncodingHasCorrectLengthAndAlphabet) {
    auto u = b::new_ulid();
    EXPECT_EQ(u.size(), 26u);
    for (char c : u) {
        EXPECT_NE(std::string_view{b::k_crockford_alphabet}.find(c),
                  std::string_view::npos)
            << "char '" << c << "' not in Crockford alphabet";
    }
}

TEST(Ulid, FirstCharIsZeroToSeven) {
    // The 128-bit ULID gets 2 synthetic leading zero bits to round up
    // to 130 = 26*5. So the first character's high 2 bits are always
    // zero, leaving only the low 3 bits encoded — i.e., '0'..'7'.
    for (int i = 0; i < 32; ++i) {
        const auto u = b::new_ulid();
        EXPECT_LE(u[0], '7') << "iteration " << i << " produced '" << u[0] << "'";
        EXPECT_GE(u[0], '0');
    }
}

TEST(Ulid, IsTimeOrderedBetweenCalls) {
    // Two ULIDs generated back-to-back must be lex-comparable (later
    // ULID >= earlier ULID, modulo same-millisecond ties where the
    // random tail decides). Test a sequence with a small sleep so the
    // timestamp portion changes for sure.
    const auto a = b::new_ulid();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto b_ = b::new_ulid();
    EXPECT_LT(a, b_);
}

TEST(Ulid, EncoderKnownFixture) {
    // 16 zero bytes → all '0's.
    std::array<std::uint8_t, 16> zeros{};
    EXPECT_EQ(b::encode_ulid(zeros), std::string(26, '0'));

    // Highest possible bytes → 7ZZZZZZZZZZZZZZZZZZZZZZZZZ
    // (first char limited to 7 because of the 2 synthetic leading
    // zero bits; remaining 25 chars are all 'Z').
    std::array<std::uint8_t, 16> highs;
    highs.fill(0xFF);
    auto u = b::encode_ulid(highs);
    EXPECT_EQ(u[0], '7');
    for (std::size_t i = 1; i < u.size(); ++i) EXPECT_EQ(u[i], 'Z');
}

// --- Block::create_from_series ---------------------------------------------

TEST(BlockCreate, EmptyInputProducesEmptyBlock) {
    TempDir tmp;
    auto dir_or = b::Block::create_from_series(tmp.path(), {});
    ASSERT_TRUE(dir_or.has_value()) << dir_or.error().message();
    auto blk_or = b::Block::open(*dir_or);
    ASSERT_TRUE(blk_or.has_value());
    EXPECT_EQ(blk_or->meta().stats.num_series, 0u);
    EXPECT_EQ(blk_or->meta().stats.num_chunks, 0u);
    EXPECT_EQ(blk_or->meta().stats.num_samples, 0u);
}

TEST(BlockCreate, SingleSeriesRoundtrip) {
    TempDir tmp;
    std::vector<b::Block::SeriesInput> input;
    {
        b::Block::SeriesInput s;
        s.labels = m::Labels{{"__name__", "test_metric"}, {"instance", "host1"}};
        s.chunks.push_back(make_chunk({{100, 1.5}, {200, 2.5}, {300, 3.5}}));
        input.push_back(std::move(s));
    }

    auto dir = *b::Block::create_from_series(tmp.path(), input);

    auto blk = *b::Block::open(dir);
    EXPECT_EQ(blk.meta().stats.num_series, 1u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 1u);
    EXPECT_EQ(blk.meta().stats.num_samples, 3u);
    EXPECT_EQ(blk.meta().min_time, 100);
    EXPECT_EQ(blk.meta().max_time, 300);
    EXPECT_EQ(blk.meta().compaction.level, 1);
    ASSERT_EQ(blk.meta().compaction.sources.size(), 1u);
    EXPECT_EQ(blk.meta().compaction.sources[0], blk.meta().ulid);

    auto results = blk.query("__name__", "test_metric");
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    const auto& r = (*results)[0];
    EXPECT_EQ(r.labels.get("__name__"), std::optional<std::string_view>{"test_metric"});
    EXPECT_EQ(r.labels.get("instance"), std::optional<std::string_view>{"host1"});
    ASSERT_EQ(r.chunks.size(), 1u);
    EXPECT_EQ(r.chunks[0].num_samples(), 3);

    auto it = r.chunks[0].iterator();
    std::vector<std::pair<std::int64_t, double>> got;
    while (it.next()) got.emplace_back(it.t(), it.v());
    EXPECT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].first, 100);
    EXPECT_DOUBLE_EQ(got[0].second, 1.5);
    EXPECT_EQ(got[2].first, 300);
    EXPECT_DOUBLE_EQ(got[2].second, 3.5);
}

TEST(BlockCreate, MultipleSeriesEachWithMultipleChunks) {
    TempDir tmp;
    std::vector<b::Block::SeriesInput> input;
    for (int i = 0; i < 5; ++i) {
        b::Block::SeriesInput s;
        s.labels = m::Labels{
            {"__name__", "metric"},
            {"id",       std::to_string(i)},
        };
        s.chunks.push_back(make_chunk({{0, static_cast<double>(i)}, {100, static_cast<double>(i + 1)}}));
        s.chunks.push_back(make_chunk({{200, static_cast<double>(i + 2)}, {300, static_cast<double>(i + 3)}}));
        input.push_back(std::move(s));
    }

    auto dir = *b::Block::create_from_series(tmp.path(), input);
    auto blk = *b::Block::open(dir);
    EXPECT_EQ(blk.meta().stats.num_series, 5u);
    EXPECT_EQ(blk.meta().stats.num_chunks, 10u);   // 5 series × 2 chunks
    EXPECT_EQ(blk.meta().stats.num_samples, 20u);  // 5 series × 2 chunks × 2 samples

    auto results = blk.query("__name__", "metric");
    ASSERT_TRUE(results.has_value());
    EXPECT_EQ(results->size(), 5u);
    // Each series has 2 decoded chunks.
    for (const auto& r : *results) {
        EXPECT_EQ(r.chunks.size(), 2u);
    }

    // A specific id lookup must return exactly one series with the right
    // id label.
    auto single = blk.query("id", "3");
    ASSERT_TRUE(single.has_value());
    ASSERT_EQ(single->size(), 1u);
    EXPECT_EQ((*single)[0].labels.get("id"),
              std::optional<std::string_view>{"3"});
}

TEST(BlockCreate, BlockDirIsUlidNamed) {
    TempDir tmp;
    auto dir = *b::Block::create_from_series(tmp.path(), {});
    const auto name = dir.filename().string();
    EXPECT_EQ(name.size(), 26u);
    for (char c : name) {
        EXPECT_NE(std::string_view{b::k_crockford_alphabet}.find(c),
                  std::string_view::npos);
    }
}

TEST(BlockCreate, TombstonesFileIsPresentAndEmpty) {
    TempDir tmp;
    auto dir = *b::Block::create_from_series(tmp.path(), {});
    auto ts = dir / "tombstones";
    EXPECT_TRUE(std::filesystem::exists(ts));
    EXPECT_EQ(std::filesystem::file_size(ts), 0u);
}

TEST(BlockCreate, CrossValidationAggregateQueryRecoversAllSeries) {
    // The "every series is reachable" guarantee the read path proves
    // for upstream-produced blocks should also hold for blocks WE
    // produce. Build a meaningful test set and verify.
    TempDir tmp;
    std::vector<b::Block::SeriesInput> input;
    for (int i = 0; i < 20; ++i) {
        b::Block::SeriesInput s;
        s.labels = m::Labels{
            {"__name__", "node_metric"},
            {"job",      i % 2 == 0 ? "even" : "odd"},
            {"instance", std::to_string(i)},
        };
        s.chunks.push_back(make_chunk({{static_cast<std::int64_t>(i * 100), 1.0}}));
        input.push_back(std::move(s));
    }
    auto dir = *b::Block::create_from_series(tmp.path(), input);
    auto blk = *b::Block::open(dir);

    // Aggregate across every (name, value) entry in the postings table
    // and union the labels. Should recover exactly 20 distinct series.
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
    EXPECT_EQ(seen.size(), 20u);
}
