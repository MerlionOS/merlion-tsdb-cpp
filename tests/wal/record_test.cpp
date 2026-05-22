#include "merlion_tsdb/wal/record.hpp"

#include <bit>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/wal/segment_reader.hpp"
#include "merlion_tsdb/wal/segment_writer.hpp"

namespace r = merlion_tsdb::wal::record;
namespace w = merlion_tsdb::wal;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "merlion_rec_%08x_%08x", rd(), rd());
        path_ = base / buf;
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

}  // namespace

// --- Series tests -----------------------------------------------------------

TEST(SeriesRecord, EmptyBatchProducesOnlyTypeByte) {
    auto enc = r::encode_series({});
    ASSERT_EQ(enc.size(), 1u);
    EXPECT_EQ(enc[0], static_cast<std::uint8_t>(r::Type::Series));
    EXPECT_EQ(r::peek_type(enc), r::Type::Series);

    auto dec = r::decode_series(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_TRUE(dec->empty());
}

TEST(SeriesRecord, SingleSeriesRoundtrip) {
    std::vector<r::RefSeries> input{
        {.ref = 42, .labels = {
            {.name = "__name__", .value = "http_requests_total"},
            {.name = "job",      .value = "api"},
            {.name = "instance", .value = "10.0.0.1:9090"},
        }},
    };
    auto enc = r::encode_series(input);
    auto dec = r::decode_series(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

TEST(SeriesRecord, MultipleSeriesPreserveOrderAndLabels) {
    std::vector<r::RefSeries> input;
    for (std::uint64_t i = 0; i < 50; ++i) {
        r::RefSeries s;
        s.ref = i * 100 + 1;
        s.labels.push_back({"__name__", "metric"});
        s.labels.push_back({"id", std::to_string(i)});
        if (i % 3 == 0) s.labels.push_back({"shard", "alpha"});
        input.push_back(std::move(s));
    }
    auto enc = r::encode_series(input);
    auto dec = r::decode_series(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

TEST(SeriesRecord, WrongTypeByteIsRejected) {
    std::vector<std::uint8_t> bad = {static_cast<std::uint8_t>(r::Type::Samples)};
    auto dec = r::decode_series(bad);
    ASSERT_FALSE(dec.has_value());
    EXPECT_EQ(dec.error(), r::RecordError::InvalidType);
}

TEST(SeriesRecord, TruncatedRecordIsUnexpectedEnd) {
    std::vector<r::RefSeries> input{{.ref = 1, .labels = {{"a", "b"}}}};
    auto enc = r::encode_series(input);
    enc.pop_back();
    auto dec = r::decode_series(enc);
    ASSERT_FALSE(dec.has_value());
    EXPECT_EQ(dec.error(), r::RecordError::UnexpectedEnd);
}

TEST(SeriesRecord, LargeUtf8LabelValuesRoundtrip) {
    std::vector<r::RefSeries> input{
        {.ref = 7, .labels = {
            {"foo", std::string(500, 'x')},
            {"emoji", "\xF0\x9F\x90\x9A"},  // 🐚
        }},
    };
    auto enc = r::encode_series(input);
    auto dec = r::decode_series(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

// --- SamplesV2 tests --------------------------------------------------------

TEST(SamplesV2Record, EmptyRecordRoundtripsAsEmpty) {
    auto enc = r::encode_samples_v2({});
    ASSERT_EQ(enc.size(), 1u);
    EXPECT_EQ(enc[0], static_cast<std::uint8_t>(r::Type::SamplesV2));

    auto dec = r::decode_samples_v2(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_TRUE(dec->empty());
}

TEST(SamplesV2Record, SingleSampleRoundtrip) {
    std::vector<r::RefSample> input{
        {.ref = 7, .t = 1'700'000'000'000, .st = 0, .v = 3.14},
    };
    auto enc = r::encode_samples_v2(input);
    auto dec = r::decode_samples_v2(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

TEST(SamplesV2Record, ManySamplesWithStMarkersExerciseAllBranches) {
    std::vector<r::RefSample> input{
        // first
        {.ref = 100, .t = 1'000'000, .st = 0,         .v = 1.0},
        // noST marker
        {.ref = 100, .t = 1'001'000, .st = 0,         .v = 2.0},
        // sameST: first.st=0, prev.st=0 ⇒ st=0 ⇒ noST again (we'd need
        // a non-zero history for sameST to fire). Build one below.
    };
    // Set up sameST: introduce a non-zero st, then repeat.
    input.push_back({.ref = 101, .t = 1'002'000, .st = 500, .v = 3.0});  // explicit
    input.push_back({.ref = 101, .t = 1'003'000, .st = 500, .v = 4.0});  // sameST
    input.push_back({.ref = 102, .t = 1'004'000, .st = 700, .v = 5.0});  // explicit
    input.push_back({.ref = 102, .t = 1'005'000, .st = 0,   .v = 6.0});  // noST

    auto enc = r::encode_samples_v2(input);
    auto dec = r::decode_samples_v2(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

TEST(SamplesV2Record, RefDeltaAllowsDecreasingRefs) {
    // Subsequent refs can be smaller than the previous one; the delta is
    // signed.
    std::vector<r::RefSample> input{
        {.ref = 1000, .t = 0,    .st = 0, .v = 1.0},
        {.ref = 100,  .t = 1000, .st = 0, .v = 2.0},
        {.ref = 50,   .t = 2000, .st = 0, .v = 3.0},
    };
    auto enc = r::encode_samples_v2(input);
    auto dec = r::decode_samples_v2(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

TEST(SamplesV2Record, NanAndInfRoundtrip) {
    std::vector<r::RefSample> input{
        {.ref = 1, .t = 0, .st = 0, .v = std::numeric_limits<double>::quiet_NaN()},
        {.ref = 2, .t = 1, .st = 0, .v =  std::numeric_limits<double>::infinity()},
        {.ref = 3, .t = 2, .st = 0, .v = -std::numeric_limits<double>::infinity()},
    };
    auto enc = r::encode_samples_v2(input);
    auto dec = r::decode_samples_v2(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

TEST(SamplesV2Record, WrongTypeRejected) {
    std::vector<std::uint8_t> bad = {static_cast<std::uint8_t>(r::Type::Series)};
    auto dec = r::decode_samples_v2(bad);
    ASSERT_FALSE(dec.has_value());
    EXPECT_EQ(dec.error(), r::RecordError::InvalidType);
}

TEST(SamplesV2Record, InvalidStMarkerRejected) {
    // Forge a record by hand: type + first sample + second sample with
    // a bogus marker byte (0x07 — outside {0,1,2}).
    std::vector<std::uint8_t> rec;
    rec.push_back(static_cast<std::uint8_t>(r::Type::SamplesV2));
    // First sample (ref=0, t=0, st=0, v=0) — all zero zigzag varints +
    // 8 BE bytes for the value.
    rec.push_back(0x00);  // ref
    rec.push_back(0x00);  // t
    rec.push_back(0x00);  // st
    for (int i = 0; i < 8; ++i) rec.push_back(0x00);  // value bits
    // Second sample: ref_delta=0, t_delta=0, marker=0x07 (invalid)
    rec.push_back(0x00);
    rec.push_back(0x00);
    rec.push_back(0x07);
    for (int i = 0; i < 8; ++i) rec.push_back(0x00);
    auto dec = r::decode_samples_v2(rec);
    ASSERT_FALSE(dec.has_value());
    EXPECT_EQ(dec.error(), r::RecordError::StMarkerInvalid);
}

TEST(SamplesV2Record, FuzzRoundtrip) {
    std::mt19937_64 rng(0xFEEDFACECAFEBEEFULL);
    std::vector<r::RefSample> input;
    std::int64_t t = 1'700'000'000'000;
    r::SeriesRef ref = 1;
    for (int i = 0; i < 300; ++i) {
        ref += static_cast<r::SeriesRef>(rng() % 5);
        t   += static_cast<std::int64_t>(rng() % 10'000);
        std::int64_t st = 0;
        switch (rng() % 4) {
            case 0: st = 0; break;
            case 1: st = input.empty() ? 0 : input.back().st; break;
            case 2: st = t - static_cast<std::int64_t>(rng() % 1'000'000); break;
            default: st = static_cast<std::int64_t>(rng());
        }
        double v = std::bit_cast<double>(rng());
        input.push_back({.ref = ref, .t = t, .st = st, .v = v});
    }
    auto enc = r::encode_samples_v2(input);
    auto dec = r::decode_samples_v2(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, input);
}

// --- End-to-end through the WAL ---------------------------------------------

TEST(WalEndToEnd, SeriesAndSamplesV2ThroughSegmentWriter) {
    TempDir tmp;
    std::vector<r::RefSeries> series{
        {.ref = 1, .labels = {{"__name__", "metric_a"}}},
        {.ref = 2, .labels = {{"__name__", "metric_b"}, {"job", "demo"}}},
    };
    std::vector<r::RefSample> samples{
        {.ref = 1, .t = 100, .st = 0, .v = 1.5},
        {.ref = 2, .t = 100, .st = 0, .v = 2.5},
        {.ref = 1, .t = 200, .st = 0, .v = 3.5},
    };

    {
        auto sw = *w::SegmentWriter::open(tmp.path());
        ASSERT_TRUE(sw.log(r::encode_series(series)).has_value());
        ASSERT_TRUE(sw.log(r::encode_samples_v2(samples)).has_value());
        ASSERT_TRUE(sw.cut().has_value());
    }

    auto sr = *w::SegmentReader::open(tmp.path());
    auto rec0 = sr.next();
    ASSERT_TRUE(rec0.has_value());
    ASSERT_EQ(r::peek_type(*rec0), r::Type::Series);
    auto got_series = r::decode_series(*rec0);
    ASSERT_TRUE(got_series.has_value());
    EXPECT_EQ(*got_series, series);

    auto rec1 = sr.next();
    ASSERT_TRUE(rec1.has_value());
    ASSERT_EQ(r::peek_type(*rec1), r::Type::SamplesV2);
    auto got_samples = r::decode_samples_v2(*rec1);
    ASSERT_TRUE(got_samples.has_value());
    EXPECT_EQ(*got_samples, samples);

    auto end = sr.next();
    ASSERT_FALSE(end.has_value());
    EXPECT_EQ(end.error(), w::SegmentReadError::EndOfStream);
}
