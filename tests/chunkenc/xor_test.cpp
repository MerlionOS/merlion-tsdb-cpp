#include "merlion_tsdb/chunkenc/xor.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace c = merlion_tsdb::chunkenc;

namespace {

struct Sample {
    std::int64_t t;
    double v;
};

// Builds a chunk by appending the given samples. Returns the chunk and a
// guarantee that all appends succeeded.
c::XORChunk build_chunk(std::span<const Sample> samples) {
    c::XORChunk chunk;
    auto app = chunk.appender();
    EXPECT_TRUE(app.has_value());
    for (const auto& s : samples) {
        EXPECT_TRUE(app->append(s.t, s.v));
    }
    return chunk;
}

// Iterates a chunk and returns the recovered samples. Asserts no error.
std::vector<Sample> read_all(const c::XORChunk& chunk) {
    std::vector<Sample> out;
    auto it = chunk.iterator();
    while (it.next()) {
        out.push_back({it.t(), it.v()});
    }
    EXPECT_FALSE(it.error().has_value());
    return out;
}

void expect_equal(const std::vector<Sample>& got, std::span<const Sample> want) {
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].t, want[i].t) << "i=" << i;
        // Use bit_cast comparison so NaN samples round-trip too.
        EXPECT_EQ(std::bit_cast<std::uint64_t>(got[i].v),
                  std::bit_cast<std::uint64_t>(want[i].v)) << "i=" << i;
    }
}

}  // namespace

TEST(XORChunk, EmptyChunkHasTwoByteHeaderAndZeroSamples) {
    c::XORChunk chunk;
    ASSERT_EQ(chunk.bytes().size(), 2u);
    EXPECT_EQ(chunk.bytes()[0], 0x00);
    EXPECT_EQ(chunk.bytes()[1], 0x00);
    EXPECT_EQ(chunk.num_samples(), 0);
    EXPECT_EQ(chunk.encoding(), c::Encoding::XOR);
}

TEST(XORChunk, SingleSampleRoundtrip) {
    const std::array<Sample, 1> samples{{{100, 3.14}}};
    auto chunk = build_chunk(samples);
    EXPECT_EQ(chunk.num_samples(), 1);
    expect_equal(read_all(chunk), samples);
}

TEST(XORChunk, TwoSamplesRoundtrip) {
    const std::array<Sample, 2> samples{{{100, 1.0}, {200, 2.0}}};
    auto chunk = build_chunk(samples);
    EXPECT_EQ(chunk.num_samples(), 2);
    expect_equal(read_all(chunk), samples);
}

TEST(XORChunk, ConstantValueCompresses) {
    // All values equal → every xor delta is 0 → 1 bit per value sample after
    // the first.  Useful regression test for the delta==0 fast path.
    std::array<Sample, 100> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = {static_cast<std::int64_t>(i) * 1000, 42.0};
    }
    auto chunk = build_chunk(samples);
    EXPECT_EQ(chunk.num_samples(), samples.size());
    expect_equal(read_all(chunk), samples);
    // Sanity: should compress dramatically.  Header(2) + varint(t) +
    // 64-bit first value + tiny ~100 bits of follow-up.
    EXPECT_LT(chunk.bytes().size(), 64u);
}

TEST(XORChunk, RegularIntervalDoDIsZero) {
    // Strictly periodic timestamps -> all dods are 0 after sample 1.
    // Forces the '0' prefix path in the encoder.
    std::array<Sample, 50> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = {static_cast<std::int64_t>(i) * 15'000,
                      std::sin(static_cast<double>(i) * 0.1)};
    }
    auto chunk = build_chunk(samples);
    expect_equal(read_all(chunk), samples);
}

TEST(XORChunk, IrregularDoDExercisesAllBuckets) {
    // Hand-picked timestamps that walk through every dod-prefix bucket so
    // every encoder branch executes at least once. Values are picked to
    // exercise both the 'reuse window' and 'new window' xor_write paths.
    std::array<Sample, 8> samples{{
        {0,                  1.0},     // first sample (raw)
        {1'000,              1.0},     // tDelta=1000 (uvarint)
        {2'000,              1.0},     // dod=0     -> prefix '0'    + delta=0
        {3'050,              2.0},     // dod=50    -> prefix '10'   + value change
        {4'200,              2.0},     // dod=100   -> prefix '10'   + delta=0
        {5'500,              2.0001},  // dod=150   -> prefix '10'   + tiny xor
        {7'000'000'500,      3.0},     // dod is huge -> prefix '1111' + 64-bit
        {7'000'001'600,      3.0},     // dod small again -> '10'
    }};
    auto chunk = build_chunk(samples);
    EXPECT_EQ(chunk.num_samples(), samples.size());
    expect_equal(read_all(chunk), samples);
}

TEST(XORChunk, NegativeDoDRoundtrips) {
    // Timestamps decelerating (gaps shrink) produce negative dods.
    std::array<Sample, 5> samples{{
        {0,      10.0},
        {1'000,  10.0},
        {2'500,  10.0},   // tDelta=1500
        {3'700,  10.0},   // tDelta=1200, dod = -300 (negative, fits in 14 bits)
        {4'600,  10.0},   // tDelta=900,  dod = -300
    }};
    auto chunk = build_chunk(samples);
    expect_equal(read_all(chunk), samples);
}

TEST(XORChunk, RandomRoundtripLarge) {
    // Many samples with random t-deltas and values. The fuzz exercise that
    // matches Go's TestChunk behaviour for the XOR encoder.
    std::mt19937_64 rng(0xC0DECAFE);
    std::vector<Sample> samples;
    std::int64_t t = 1'700'000'000'000;
    double v = 0.0;
    for (int i = 0; i < 500; ++i) {
        t += static_cast<std::int64_t>(rng() % 20'000);
        // Mix of small float deltas, big jumps, and the occasional NaN/Inf.
        const auto pick = rng() % 32;
        if (pick == 0) v = std::numeric_limits<double>::quiet_NaN();
        else if (pick == 1) v = std::numeric_limits<double>::infinity();
        else if (pick == 2) v = -std::numeric_limits<double>::infinity();
        else if (pick < 8) v += static_cast<double>(rng() % 1000) * 0.001;
        else v = std::bit_cast<double>(rng());
        samples.push_back({t, v});
    }
    auto chunk = build_chunk(samples);
    EXPECT_EQ(chunk.num_samples(), samples.size());
    expect_equal(read_all(chunk), samples);
}

TEST(XORChunk, AppenderFromNonEmptyChunkReplaysState) {
    // Append in two batches with appender() recreated mid-way. The second
    // appender must recover (t, v, t_delta, leading, trailing) by replaying
    // the existing samples — if any of that state is wrong, decoded samples
    // after the boundary will mismatch.
    const std::array<Sample, 4> first_batch{{
        {0, 1.0}, {1'000, 1.0}, {2'000, 1.5}, {3'000, 1.7},
    }};
    const std::array<Sample, 3> second_batch{{
        {4'100, 1.9},   // dod = 100
        {5'200, 2.0},   // dod = 0
        {6'500, -3.0},  // dod = 200, big value jump
    }};

    c::XORChunk chunk;
    {
        auto app = chunk.appender();
        ASSERT_TRUE(app.has_value());
        for (const auto& s : first_batch) ASSERT_TRUE(app->append(s.t, s.v));
    }
    {
        auto app = chunk.appender();
        ASSERT_TRUE(app.has_value());
        for (const auto& s : second_batch) ASSERT_TRUE(app->append(s.t, s.v));
    }

    std::vector<Sample> expected;
    expected.insert(expected.end(), first_batch.begin(),  first_batch.end());
    expected.insert(expected.end(), second_batch.begin(), second_batch.end());

    EXPECT_EQ(chunk.num_samples(), expected.size());
    expect_equal(read_all(chunk), expected);
}

TEST(XORChunk, IteratorOnEmptyChunkReturnsFalseAndNoError) {
    c::XORChunk chunk;
    auto it = chunk.iterator();
    EXPECT_FALSE(it.next());
    EXPECT_FALSE(it.error().has_value());
    EXPECT_EQ(it.num_total(), 0);
}

TEST(XORChunk, FromBytesRoundtripPreservesData) {
    const std::array<Sample, 6> samples{{
        {0, 1.0}, {100, 1.5}, {200, 1.5}, {300, 2.0},
        {500, -7.0}, {800, std::numeric_limits<double>::quiet_NaN()},
    }};
    auto original = build_chunk(samples);
    auto bytes = std::vector<std::uint8_t>(original.bytes().begin(),
                                           original.bytes().end());

    auto adopted = c::XORChunk::from_bytes(std::move(bytes));
    EXPECT_EQ(adopted.num_samples(), samples.size());
    expect_equal(read_all(adopted), samples);
}

TEST(XORChunk, CompactDoesNotChangeData) {
    std::array<Sample, 10> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = {static_cast<std::int64_t>(i) * 1000, static_cast<double>(i)};
    }
    auto chunk = build_chunk(samples);
    const auto bytes_before = std::vector<std::uint8_t>(
        chunk.bytes().begin(), chunk.bytes().end());
    chunk.compact();
    const auto bytes_after = std::vector<std::uint8_t>(
        chunk.bytes().begin(), chunk.bytes().end());
    EXPECT_EQ(bytes_before, bytes_after);
    expect_equal(read_all(chunk), samples);
}
