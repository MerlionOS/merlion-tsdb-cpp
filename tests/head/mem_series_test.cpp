#include "merlion_tsdb/head/mem_series.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace h = merlion_tsdb::head;
namespace m = merlion_tsdb::model;

namespace {

// Replays a series' chunks via the XOR iterator and returns the recovered
// (t, v) pairs.
std::vector<std::pair<std::int64_t, double>>
read_all_samples(const h::MemSeries& s) {
    std::vector<std::pair<std::int64_t, double>> out;
    for (const auto* c : s.chunks()) {
        auto it = c->iterator();
        while (it.next()) out.emplace_back(it.t(), it.v());
        EXPECT_FALSE(it.error().has_value());
    }
    return out;
}

}  // namespace

TEST(MemSeries, FreshSeriesIsEmpty) {
    h::MemSeries s(1, m::Labels{{"__name__", "metric"}});
    EXPECT_EQ(s.ref(), 1u);
    EXPECT_EQ(s.num_chunks(), 0u);
    EXPECT_EQ(s.num_samples(), 0u);
    EXPECT_TRUE(s.chunks().empty());
}

TEST(MemSeries, SingleAppendCreatesOneChunk) {
    h::MemSeries s(1, m::Labels{{"a", "1"}});
    EXPECT_TRUE(s.append(100, 3.14));
    EXPECT_EQ(s.num_chunks(), 1u);
    EXPECT_EQ(s.num_samples(), 1u);
    EXPECT_EQ(s.last_t(), 100);
    EXPECT_DOUBLE_EQ(s.last_value(), 3.14);

    auto recovered = read_all_samples(s);
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered[0].first, 100);
    EXPECT_DOUBLE_EQ(recovered[0].second, 3.14);
}

TEST(MemSeries, MonotonicTimestampsRequired) {
    h::MemSeries s(1, m::Labels{});
    EXPECT_TRUE(s.append(100, 1.0));
    EXPECT_TRUE(s.append(101, 2.0));
    EXPECT_TRUE(s.append(101, 3.0));   // same timestamp ok
    EXPECT_FALSE(s.append(100, 4.0));  // older — rejected
    EXPECT_EQ(s.num_samples(), 3u);
    EXPECT_EQ(s.last_t(), 101);
    EXPECT_DOUBLE_EQ(s.last_value(), 3.0);
}

TEST(MemSeries, ManySamplesRecoverInOrder) {
    h::MemSeries s(7, m::Labels{{"foo", "bar"}});
    std::vector<std::pair<std::int64_t, double>> expected;
    for (int i = 0; i < 500; ++i) {
        const std::int64_t t = i * 1000;
        const double v = static_cast<double>(i) * 0.5;
        expected.emplace_back(t, v);
        ASSERT_TRUE(s.append(t, v));
    }
    EXPECT_EQ(s.num_samples(), 500u);
    EXPECT_EQ(read_all_samples(s), expected);
}

TEST(MemSeries, ChunksAreCutBeforeXorChunkOverflows) {
    h::MemSeries s(1, m::Labels{});
    // Append enough samples to span many chunks. With a 1024-byte cap
    // and very compressible data (constant value -> ~1 bit per sample),
    // a single chunk holds tens of thousands of samples; force the cut
    // by using random doubles instead.
    std::mt19937_64 rng(0x1234567);
    for (int i = 0; i < 5000; ++i) {
        ASSERT_TRUE(s.append(i, std::bit_cast<double>(rng())));
    }
    EXPECT_GT(s.num_chunks(), 1u);
    EXPECT_EQ(s.num_samples(), 5000u);
    // Each chunk must respect the soft cap.
    for (const auto* c : s.chunks()) {
        EXPECT_LE(c->bytes().size(), merlion_tsdb::chunkenc::XORChunk::kMaxBytes)
            << "chunk too large";
    }
}

TEST(MemSeries, ResetClearsState) {
    h::MemSeries s(1, m::Labels{});
    s.append(100, 1.0);
    s.append(200, 2.0);
    EXPECT_GT(s.num_samples(), 0u);
    s.reset();
    EXPECT_EQ(s.num_samples(), 0u);
    EXPECT_EQ(s.num_chunks(), 0u);
    // After reset, append works again.
    EXPECT_TRUE(s.append(0, 1.0));
    EXPECT_EQ(s.num_samples(), 1u);
}
