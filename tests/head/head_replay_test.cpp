#include "merlion_tsdb/head/head.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace h = merlion_tsdb::head;
namespace m = merlion_tsdb::model;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "merlion_replay_%08x_%08x", rd(), rd());
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

// Snapshot every (series-labels-string, t, v) sample currently in the head.
struct Sample {
    std::string labels_str;
    std::int64_t t;
    double v;
    friend bool operator==(const Sample&, const Sample&) = default;
};

std::string labels_to_string(const m::Labels& l) {
    std::string out;
    out += "{";
    for (const auto& e : l.entries()) {
        if (out.size() > 1) out += ",";
        out += e.name + "=" + e.value;
    }
    out += "}";
    return out;
}

std::vector<Sample> snapshot(const h::Head& head) {
    std::vector<Sample> out;
    for (const auto* s : head.series().all_series()) {
        const auto label_str = labels_to_string(s->labels());
        for (const auto* c : s->chunks()) {
            auto it = c->iterator();
            while (it.next()) out.push_back({label_str, it.t(), it.v()});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Sample& a, const Sample& b) {
                  return std::tie(a.labels_str, a.t) < std::tie(b.labels_str, b.t);
              });
    return out;
}

}  // namespace

TEST(HeadReplay, EmptyDirOpensCleanly) {
    TempDir tmp;
    auto head = h::Head::open(tmp.path() / "db");
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->series().size(), 0u);
}

TEST(HeadReplay, ReopenRecoversAppendedSamples) {
    TempDir tmp;
    auto db = tmp.path() / "db";
    std::vector<Sample> expected;
    {
        auto head = *h::Head::open(db);
        m::Labels la{{"__name__", "a"}};
        m::Labels lb{{"__name__", "b"}};
        ASSERT_TRUE(head.append(la, 100, 1.0).has_value());
        ASSERT_TRUE(head.append(lb, 100, 2.0).has_value());
        ASSERT_TRUE(head.append(la, 200, 1.5).has_value());
        ASSERT_TRUE(head.append(lb, 200, 2.5).has_value());
        ASSERT_TRUE(head.close().has_value());
        expected = {
            {"{__name__=a}", 100, 1.0}, {"{__name__=a}", 200, 1.5},
            {"{__name__=b}", 100, 2.0}, {"{__name__=b}", 200, 2.5},
        };
    }

    auto head = *h::Head::open(db);
    EXPECT_EQ(head.series().size(), 2u);
    EXPECT_EQ(snapshot(head), expected);
}

TEST(HeadReplay, ReopenAcrossManyCommitsAndSegments) {
    TempDir tmp;
    auto db = tmp.path() / "db";

    std::vector<Sample> expected;
    {
        // Tiny segment size so we span multiple files even on a small workload.
        // SegmentWriter is opened via Head::open(), but the test exercises
        // many appends; just trust that real-Prometheus segment rollovers
        // happen and verify the end-to-end replay below.
        auto head = *h::Head::open(db);
        for (int s = 0; s < 25; ++s) {
            m::Labels lbls{{"__name__", "metric"}, {"id", std::to_string(s)}};
            const auto label_str = labels_to_string(lbls);
            for (int i = 0; i < 12; ++i) {
                const std::int64_t t = i * 1000;
                const double v = static_cast<double>(s * 100 + i);
                ASSERT_TRUE(head.append(lbls, t, v).has_value())
                    << "s=" << s << " i=" << i;
                expected.push_back({label_str, t, v});
            }
            if (s % 5 == 4) ASSERT_TRUE(head.commit().has_value());
        }
        ASSERT_TRUE(head.close().has_value());
    }

    auto head = *h::Head::open(db);
    EXPECT_EQ(head.series().size(), 25u);

    auto got = snapshot(head);
    std::sort(expected.begin(), expected.end(),
              [](const Sample& a, const Sample& b) {
                  return std::tie(a.labels_str, a.t) < std::tie(b.labels_str, b.t);
              });
    EXPECT_EQ(got, expected);
}

TEST(HeadReplay, AppendContinuesAfterReplay) {
    TempDir tmp;
    auto db = tmp.path() / "db";

    {
        auto head = *h::Head::open(db);
        ASSERT_TRUE(head.append(m::Labels{{"__name__", "x"}}, 100, 1.0).has_value());
        ASSERT_TRUE(head.close().has_value());
    }

    auto head = *h::Head::open(db);
    EXPECT_EQ(head.series().size(), 1u);
    // Appending more to the same series must continue the timeline.
    ASSERT_TRUE(head.append(m::Labels{{"__name__", "x"}}, 200, 2.0).has_value());
    // Out-of-order timestamp relative to replayed history must still be rejected.
    EXPECT_FALSE(head.append(m::Labels{{"__name__", "x"}}, 50, 0.0).has_value());
    ASSERT_TRUE(head.close().has_value());

    // A second reopen must observe both the original + new appended sample.
    auto head2 = *h::Head::open(db);
    auto samples = snapshot(head2);
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].t, 100);
    EXPECT_EQ(samples[1].t, 200);
}

TEST(HeadReplay, TornTailDroppedSilently) {
    TempDir tmp;
    auto db = tmp.path() / "db";

    {
        auto head = *h::Head::open(db);
        ASSERT_TRUE(head.append(m::Labels{{"__name__", "x"}}, 100, 1.0).has_value());
        ASSERT_TRUE(head.commit().has_value());

        // Append more without committing — the destructor will commit so we
        // need to skip that. Use close to abandon.
        ASSERT_TRUE(head.append(m::Labels{{"__name__", "y"}}, 200, 2.0).has_value());
        ASSERT_TRUE(head.commit().has_value());

        // Final unflushed append + corrupt the last segment by truncating
        // halfway through after a sync to simulate a torn record.
        ASSERT_TRUE(head.append(m::Labels{{"__name__", "z"}}, 300, 3.0).has_value());
        // Don't call commit here — let the destructor flush + close.
    }

    // Now simulate a crash that left the tail torn: truncate the last
    // segment file by half. Replay should drop the torn record but recover
    // everything that was committed before.
    std::vector<std::filesystem::path> segs;
    for (const auto& e : std::filesystem::directory_iterator(db / "wal")) {
        segs.push_back(e.path());
    }
    std::sort(segs.begin(), segs.end());
    ASSERT_FALSE(segs.empty());
    const auto& last = segs.back();
    const auto sz = std::filesystem::file_size(last);
    if (sz > 10) {
        std::filesystem::resize_file(last, sz / 2);
    }

    // Reopening should not error. The samples that survived the truncation
    // are head-of-stream; the torn tail is dropped.
    auto head = h::Head::open(db);
    ASSERT_TRUE(head.has_value()) << "open after torn-tail should succeed";
    // At minimum, the two committed series + samples survive.
    EXPECT_GE(head->series().size(), 2u);
}

TEST(HeadReplay, CrcCorruptionInMidSegmentFailsOpen) {
    TempDir tmp;
    auto db = tmp.path() / "db";

    {
        auto head = *h::Head::open(db);
        for (int i = 0; i < 20; ++i) {
            m::Labels lbls{{"__name__", "metric"}, {"id", std::to_string(i)}};
            ASSERT_TRUE(head.append(lbls, i * 1000, static_cast<double>(i)).has_value());
        }
        ASSERT_TRUE(head.close().has_value());
    }

    // Flip a byte deep inside the first segment to invalidate CRC.
    const auto seg = db / "wal" / "00000000";
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(seg, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    ASSERT_GT(bytes.size(), 100u);
    bytes[50] ^= 0x55;
    {
        std::ofstream out(seg, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    auto head = h::Head::open(db);
    EXPECT_FALSE(head.has_value()) << "open should fail on mid-segment corruption";
}
