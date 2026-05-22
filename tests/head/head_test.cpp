#include "merlion_tsdb/head/head.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/wal/record.hpp"
#include "merlion_tsdb/wal/segment_reader.hpp"

namespace h = merlion_tsdb::head;
namespace m = merlion_tsdb::model;
namespace w = merlion_tsdb::wal;
namespace r = merlion_tsdb::wal::record;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "merlion_head_%08x_%08x", rd(), rd());
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

// Reads all WAL records out of `wal_dir`, segmented by record type.
struct WalDump {
    std::vector<r::RefSeries> series;
    std::vector<r::RefSample> samples;
};

WalDump dump_wal(const std::filesystem::path& wal_dir) {
    WalDump out;
    auto rd = *w::SegmentReader::open(wal_dir);
    while (true) {
        auto rec = rd.next();
        if (!rec) break;
        switch (r::peek_type(*rec)) {
            case r::Type::Series: {
                auto s = r::decode_series(*rec);
                EXPECT_TRUE(s.has_value());
                if (s) out.series.insert(out.series.end(), s->begin(), s->end());
                break;
            }
            case r::Type::SamplesV2: {
                auto s = r::decode_samples_v2(*rec);
                EXPECT_TRUE(s.has_value());
                if (s) out.samples.insert(out.samples.end(), s->begin(), s->end());
                break;
            }
            default:
                ADD_FAILURE() << "unexpected record type: "
                              << static_cast<int>(r::peek_type(*rec));
                break;
        }
    }
    return out;
}

}  // namespace

TEST(Head, OpenCreatesDirAndWal) {
    TempDir tmp;
    auto path = tmp.path() / "db";
    auto head = h::Head::open(path);
    ASSERT_TRUE(head.has_value());
    EXPECT_TRUE(std::filesystem::is_directory(path / "wal"));
    EXPECT_TRUE(std::filesystem::exists(path / "wal" / "00000000"));
    EXPECT_EQ(head->pending_series(), 0u);
    EXPECT_EQ(head->pending_samples(), 0u);
}

TEST(Head, AppendBuffersUntilCommit) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    auto ref = head.append(m::Labels{{"__name__", "metric"}}, 100, 1.0);
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(*ref, 1u);
    EXPECT_EQ(head.pending_series(), 1u);
    EXPECT_EQ(head.pending_samples(), 1u);
    // Without commit, the WAL on disk should be empty (no records yet —
    // just the file exists).
    auto dump = dump_wal(tmp.path() / "wal");
    EXPECT_TRUE(dump.series.empty());
    EXPECT_TRUE(dump.samples.empty());
}

TEST(Head, CommitFlushesPendingRecords) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    ASSERT_TRUE(head.append(m::Labels{{"__name__", "a"}}, 100, 1.0).has_value());
    ASSERT_TRUE(head.append(m::Labels{{"__name__", "b"}}, 101, 2.0).has_value());
    ASSERT_TRUE(head.commit().has_value());
    EXPECT_EQ(head.pending_series(), 0u);
    EXPECT_EQ(head.pending_samples(), 0u);

    auto dump = dump_wal(tmp.path() / "wal");
    ASSERT_EQ(dump.series.size(), 2u);
    EXPECT_EQ(dump.series[0].ref, 1u);
    EXPECT_EQ(dump.series[1].ref, 2u);
    ASSERT_EQ(dump.samples.size(), 2u);
    EXPECT_EQ(dump.samples[0].ref, 1u);
    EXPECT_EQ(dump.samples[0].t, 100);
    EXPECT_EQ(dump.samples[1].ref, 2u);
    EXPECT_EQ(dump.samples[1].t, 101);
}

TEST(Head, RepeatedAppendDoesNotReEmitSeriesRecord) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    m::Labels lbls{{"__name__", "metric"}};
    ASSERT_TRUE(head.append(lbls, 100, 1.0).has_value());
    ASSERT_TRUE(head.commit().has_value());

    // Append more samples to the same series across two more commits.
    ASSERT_TRUE(head.append(lbls, 200, 2.0).has_value());
    ASSERT_TRUE(head.commit().has_value());
    ASSERT_TRUE(head.append(lbls, 300, 3.0).has_value());
    ASSERT_TRUE(head.commit().has_value());

    auto dump = dump_wal(tmp.path() / "wal");
    EXPECT_EQ(dump.series.size(), 1u) << "series should be emitted only once";
    EXPECT_EQ(dump.samples.size(), 3u);
    EXPECT_EQ(dump.samples[0].t, 100);
    EXPECT_EQ(dump.samples[1].t, 200);
    EXPECT_EQ(dump.samples[2].t, 300);
}

TEST(Head, OutOfOrderTimestampRejected) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    m::Labels lbls{{"__name__", "metric"}};
    ASSERT_TRUE(head.append(lbls, 200, 1.0).has_value());
    auto bad = head.append(lbls, 100, 2.0);
    EXPECT_FALSE(bad.has_value());
}

TEST(Head, CloseFlushesPendingThenForbidsOperations) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    ASSERT_TRUE(head.append(m::Labels{{"a", "1"}}, 100, 1.0).has_value());
    // No explicit commit — close should flush.
    ASSERT_TRUE(head.close().has_value());
    // Second close is a no-op.
    ASSERT_TRUE(head.close().has_value());
    // Subsequent appends fail.
    auto bad = head.append(m::Labels{{"a", "1"}}, 200, 2.0);
    EXPECT_FALSE(bad.has_value());

    // WAL should contain the buffered series + sample, flushed by close.
    auto dump = dump_wal(tmp.path() / "wal");
    ASSERT_EQ(dump.series.size(), 1u);
    ASSERT_EQ(dump.samples.size(), 1u);
    EXPECT_EQ(dump.samples[0].v, 1.0);
}

TEST(Head, DestructorImplicitlyClosesAndFlushes) {
    TempDir tmp;
    {
        auto head = *h::Head::open(tmp.path());
        ASSERT_TRUE(head.append(m::Labels{{"k", "v"}}, 42, 3.14).has_value());
        // No commit, no close — destructor must flush.
    }
    auto dump = dump_wal(tmp.path() / "wal");
    ASSERT_EQ(dump.series.size(), 1u);
    ASSERT_EQ(dump.samples.size(), 1u);
    EXPECT_EQ(dump.samples[0].v, 3.14);
}

TEST(Head, ManySeriesManyCommits) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    constexpr int n_series = 50;
    constexpr int n_samples_each = 20;

    for (int s = 0; s < n_series; ++s) {
        m::Labels lbls{{"__name__", "metric"}, {"id", std::to_string(s)}};
        for (int i = 0; i < n_samples_each; ++i) {
            ASSERT_TRUE(head.append(lbls, i * 1000, static_cast<double>(s + i)).has_value())
                << "s=" << s << " i=" << i;
        }
        // Commit every 10 series to exercise multi-record streams.
        if (s % 10 == 9) ASSERT_TRUE(head.commit().has_value());
    }
    ASSERT_TRUE(head.close().has_value());

    auto dump = dump_wal(tmp.path() / "wal");
    EXPECT_EQ(dump.series.size(), n_series);
    EXPECT_EQ(dump.samples.size(), n_series * n_samples_each);
}

TEST(Head, InMemoryStateMirrorsAppends) {
    TempDir tmp;
    auto head = *h::Head::open(tmp.path());
    auto r1 = head.append(m::Labels{{"k", "1"}}, 100, 1.0);
    auto r2 = head.append(m::Labels{{"k", "2"}}, 100, 2.0);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    const auto& store = head.series();
    EXPECT_EQ(store.size(), 2u);
    EXPECT_NE(store.get(*r1), nullptr);
    EXPECT_NE(store.get(*r2), nullptr);
    EXPECT_EQ(store.get(*r1)->num_samples(), 1u);
    EXPECT_EQ(store.get(*r1)->last_t(), 100);
    EXPECT_DOUBLE_EQ(store.get(*r1)->last_value(), 1.0);
}
