#include "merlion_tsdb/wal/segment_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/wal/page.hpp"

namespace w = merlion_tsdb::wal;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "merlion_wal_%08x_%08x", rd(), rd());
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

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::vector<std::uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()),
            reinterpret_cast<const std::uint8_t*>(s.data() + s.size())};
}

std::vector<std::vector<std::uint8_t>>
read_all_records(const std::filesystem::path& seg) {
    auto bytes = read_file(seg);
    w::PageReader reader(bytes);
    std::vector<std::vector<std::uint8_t>> out;
    while (true) {
        auto r = reader.next();
        if (!r) break;
        out.emplace_back(r->begin(), r->end());
    }
    return out;
}

}  // namespace

TEST(SegmentWriter, OpenCreatesDirAndFirstSegment) {
    TempDir tmp;
    auto sub = tmp.path() / "wal";
    auto w = w::SegmentWriter::open(sub);
    ASSERT_TRUE(w.has_value());
    EXPECT_TRUE(std::filesystem::exists(sub / "00000000"));
    EXPECT_EQ(w->current_segment_index(), 0u);
}

TEST(SegmentWriter, ResumesAtNextIndexAfterExistingSegments) {
    TempDir tmp;
    // Pre-create some segments — simulate a crashed previous run.
    std::ofstream(tmp.path() / "00000000").put('x');
    std::ofstream(tmp.path() / "00000003").put('x');
    auto w = w::SegmentWriter::open(tmp.path());
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->current_segment_index(), 4u);
    EXPECT_TRUE(std::filesystem::exists(tmp.path() / "00000004"));
}

TEST(SegmentWriter, SingleRecordRoundTrip) {
    TempDir tmp;
    auto w = *w::SegmentWriter::open(tmp.path());
    auto body = bytes_of("hello-wal");
    ASSERT_TRUE(w.log(body).has_value());
    ASSERT_TRUE(w.cut().has_value());
    auto recs = read_all_records(tmp.path() / "00000000");
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0], body);
}

TEST(SegmentWriter, MultipleRecordsInOrder) {
    TempDir tmp;
    auto w = *w::SegmentWriter::open(tmp.path());
    std::vector<std::vector<std::uint8_t>> bodies{
        bytes_of("first"),
        bytes_of("second"),
        std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF},
        {},
        std::vector<std::uint8_t>(500, 0x42),
    };
    for (const auto& b : bodies) ASSERT_TRUE(w.log(b).has_value());
    ASSERT_TRUE(w.cut().has_value());

    auto recs = read_all_records(tmp.path() / "00000000");
    EXPECT_EQ(recs, bodies);
}

TEST(SegmentWriter, RolloverWhenSegmentSizeExceeded) {
    TempDir tmp;
    // Tiny segment so rollover happens fast. Each record below frames to
    // ~7 + 100 = 107 bytes; setting segment_size to ~256 bytes forces a
    // rollover after the second or third record.
    auto w = *w::SegmentWriter::open(tmp.path(), /*segment_size*/ 256);
    for (std::size_t i = 0; i < 5; ++i) {
        std::vector<std::uint8_t> body(100, static_cast<std::uint8_t>('a' + i));
        ASSERT_TRUE(w.log(body).has_value()) << "i=" << i;
    }
    ASSERT_TRUE(w.cut().has_value());

    // Expect at least 2 segments. Records do NOT span segments — every
    // segment should contain whole records.
    std::vector<std::filesystem::path> segs;
    for (const auto& e : std::filesystem::directory_iterator(tmp.path())) {
        segs.push_back(e.path());
    }
    std::sort(segs.begin(), segs.end());
    EXPECT_GE(segs.size(), 2u);

    // Reassembled records across all segments should match what we wrote.
    std::vector<std::vector<std::uint8_t>> all;
    for (const auto& p : segs) {
        auto recs = read_all_records(p);
        all.insert(all.end(), recs.begin(), recs.end());
    }
    ASSERT_EQ(all.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(all[i],
                  std::vector<std::uint8_t>(100, static_cast<std::uint8_t>('a' + i)))
            << "i=" << i;
    }
}

TEST(SegmentWriter, RecordLargerThanSegmentStillFits) {
    TempDir tmp;
    // segment_size smaller than the record means the record gets its own
    // segment even after rollover.
    auto w = *w::SegmentWriter::open(tmp.path(), /*segment_size*/ 100);
    // First record fills the first segment by itself.
    std::vector<std::uint8_t> big_body(500, 0xAB);
    ASSERT_TRUE(w.log(big_body).has_value());
    // Second record forces rollover before logging.
    ASSERT_TRUE(w.log(bytes_of("small")).has_value());
    ASSERT_TRUE(w.cut().has_value());

    auto recs0 = read_all_records(tmp.path() / "00000000");
    auto recs1 = read_all_records(tmp.path() / "00000001");
    ASSERT_EQ(recs0.size(), 1u);
    EXPECT_EQ(recs0[0], big_body);
    ASSERT_EQ(recs1.size(), 1u);
    EXPECT_EQ(recs1[0], bytes_of("small"));
}

TEST(SegmentWriter, SyncIsIdempotent) {
    TempDir tmp;
    auto w = *w::SegmentWriter::open(tmp.path());
    ASSERT_TRUE(w.log(bytes_of("data")).has_value());
    ASSERT_TRUE(w.sync().has_value());
    ASSERT_TRUE(w.sync().has_value());
    // The on-disk file should contain the framed bytes already.
    auto bytes = read_file(tmp.path() / "00000000");
    EXPECT_GT(bytes.size(), 0u);
}

TEST(SegmentWriter, CutAdvancesIndexAndOpensFreshSegment) {
    TempDir tmp;
    auto w = *w::SegmentWriter::open(tmp.path());
    EXPECT_EQ(w.current_segment_index(), 0u);
    ASSERT_TRUE(w.log(bytes_of("a")).has_value());
    ASSERT_TRUE(w.cut().has_value());
    EXPECT_EQ(w.current_segment_index(), 1u);
    EXPECT_TRUE(std::filesystem::exists(tmp.path() / "00000001"));
}

TEST(SegmentWriter, IgnoresNonNumericFilesInDir) {
    TempDir tmp;
    std::ofstream(tmp.path() / "lock").put('x');
    std::ofstream(tmp.path() / "README").put('y');
    auto w = w::SegmentWriter::open(tmp.path());
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->current_segment_index(), 0u);
}

TEST(SegmentWriter, LargeFuzzManyRecordsManySegments) {
    TempDir tmp;
    // Segment size 64 KiB ⇒ ~2 pages each, lots of rollovers.
    auto w = *w::SegmentWriter::open(tmp.path(), /*segment_size*/ 64 * 1024);
    std::mt19937_64 rng(0xFACEFEEDC0FFEEULL);
    std::vector<std::vector<std::uint8_t>> bodies;
    for (int i = 0; i < 100; ++i) {
        const auto len = static_cast<std::size_t>(rng() % 5000);
        std::vector<std::uint8_t> b(len);
        for (auto& x : b) x = static_cast<std::uint8_t>(rng() & 0xFFU);
        bodies.push_back(b);
        ASSERT_TRUE(w.log(b).has_value()) << "i=" << i << " len=" << len;
    }
    ASSERT_TRUE(w.cut().has_value());

    // Replay all segments in order and ensure we recover identical bodies.
    std::vector<std::filesystem::path> segs;
    for (const auto& e : std::filesystem::directory_iterator(tmp.path())) {
        segs.push_back(e.path());
    }
    std::sort(segs.begin(), segs.end());

    std::vector<std::vector<std::uint8_t>> recovered;
    for (const auto& p : segs) {
        auto r = read_all_records(p);
        recovered.insert(recovered.end(), r.begin(), r.end());
    }
    EXPECT_EQ(recovered, bodies);
}
