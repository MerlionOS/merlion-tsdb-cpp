#include "merlion_tsdb/wal/segment_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/wal/segment_writer.hpp"

namespace w = merlion_tsdb::wal;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "merlion_segr_%08x_%08x", rd(), rd());
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

std::vector<std::uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()),
            reinterpret_cast<const std::uint8_t*>(s.data() + s.size())};
}

std::vector<std::vector<std::uint8_t>>
drain(w::SegmentReader& r) {
    std::vector<std::vector<std::uint8_t>> out;
    while (true) {
        auto rec = r.next();
        if (!rec) {
            EXPECT_EQ(rec.error(), w::SegmentReadError::EndOfStream);
            break;
        }
        out.emplace_back(rec->begin(), rec->end());
    }
    return out;
}

}  // namespace

TEST(SegmentReader, EmptyDirectoryIsCleanEOF) {
    TempDir tmp;
    auto r = w::SegmentReader::open(tmp.path());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->segment_count(), 0u);
    auto rec = r->next();
    ASSERT_FALSE(rec.has_value());
    EXPECT_EQ(rec.error(), w::SegmentReadError::EndOfStream);
}

TEST(SegmentReader, NonExistentDirectoryErrors) {
    auto r = w::SegmentReader::open("/tmp/this/should/not/exist/merlion-test");
    ASSERT_FALSE(r.has_value());
}

TEST(SegmentReader, SingleSegmentRoundtrip) {
    TempDir tmp;
    std::vector<std::vector<std::uint8_t>> bodies{
        bytes_of("alpha"),
        bytes_of("beta"),
        std::vector<std::uint8_t>{0x01, 0x02, 0x03},
        std::vector<std::uint8_t>(2000, 0xCD),
    };
    {
        auto wr = *w::SegmentWriter::open(tmp.path());
        for (const auto& b : bodies) ASSERT_TRUE(wr.log(b).has_value());
        ASSERT_TRUE(wr.cut().has_value());
    }
    auto rdr = *w::SegmentReader::open(tmp.path());
    EXPECT_GE(rdr.segment_count(), 1u);
    EXPECT_EQ(drain(rdr), bodies);
}

TEST(SegmentReader, MultiSegmentInOrder) {
    TempDir tmp;
    std::vector<std::vector<std::uint8_t>> bodies;
    {
        // 8 KiB segment so rollover happens every few records.
        auto wr = *w::SegmentWriter::open(tmp.path(), /*segment_size*/ 8 * 1024);
        for (int i = 0; i < 20; ++i) {
            std::vector<std::uint8_t> b(1500, static_cast<std::uint8_t>(i));
            bodies.push_back(b);
            ASSERT_TRUE(wr.log(b).has_value());
        }
        ASSERT_TRUE(wr.cut().has_value());
    }
    auto rdr = *w::SegmentReader::open(tmp.path());
    EXPECT_GT(rdr.segment_count(), 1u);
    EXPECT_EQ(drain(rdr), bodies);
}

TEST(SegmentReader, IgnoresJunkFilesInDir) {
    TempDir tmp;
    {
        auto wr = *w::SegmentWriter::open(tmp.path());
        ASSERT_TRUE(wr.log(bytes_of("only-record")).has_value());
        ASSERT_TRUE(wr.cut().has_value());
    }
    // Drop a junk file alongside the segment.
    std::ofstream(tmp.path() / "LOCK").put('x');
    std::ofstream(tmp.path() / "README").put('y');
    auto rdr = *w::SegmentReader::open(tmp.path());
    EXPECT_EQ(drain(rdr),
              std::vector<std::vector<std::uint8_t>>{bytes_of("only-record")});
}

TEST(SegmentReader, TornLastSegmentIsToleratedAsCleanEOF) {
    TempDir tmp;
    std::vector<std::vector<std::uint8_t>> bodies;
    {
        auto wr = *w::SegmentWriter::open(tmp.path());
        for (int i = 0; i < 5; ++i) {
            std::vector<std::uint8_t> b(50, static_cast<std::uint8_t>('A' + i));
            bodies.push_back(b);
            ASSERT_TRUE(wr.log(b).has_value());
        }
        // Append a record big enough to be First+Last across pages …
        std::vector<std::uint8_t> big(w::k_page_size + 1000, 0xEE);
        bodies.push_back(big);
        ASSERT_TRUE(wr.log(big).has_value());
        ASSERT_TRUE(wr.sync().has_value());
        // Destructor will fsync + close on drop. No cut() — leave the
        // segment "open" as if the process is still running.
    }
    // Now truncate the segment to simulate a crash that left the last
    // multi-page record incomplete (drop the second page).
    const auto seg = tmp.path() / "00000000";
    auto sz = std::filesystem::file_size(seg);
    ASSERT_GT(sz, w::k_page_size);
    std::filesystem::resize_file(seg, w::k_page_size);

    auto rdr = *w::SegmentReader::open(tmp.path());
    auto got = drain(rdr);
    // The first 5 records survive; the big torn one is dropped silently.
    ASSERT_EQ(got.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(got[i], bodies[i]);
    }
}

TEST(SegmentReader, TornRecordInNonLastSegmentIsCorrupt) {
    TempDir tmp;
    {
        // Produce two segments, each fully complete.
        auto wr = *w::SegmentWriter::open(tmp.path(), /*segment_size*/ 8 * 1024);
        for (int i = 0; i < 10; ++i) {
            std::vector<std::uint8_t> b(1500, static_cast<std::uint8_t>(i));
            ASSERT_TRUE(wr.log(b).has_value());
        }
        ASSERT_TRUE(wr.cut().has_value());
    }
    // Find at least two segments.
    std::vector<std::filesystem::path> segs;
    for (const auto& e : std::filesystem::directory_iterator(tmp.path())) {
        segs.push_back(e.path());
    }
    std::sort(segs.begin(), segs.end());
    ASSERT_GE(segs.size(), 2u);
    // Corrupt the FIRST segment by truncating to 3000 bytes — past the first
    // record but mid-body of the next, so the reader hits UnexpectedEnd
    // while the body is still being consumed. Cutting only 1 byte off the
    // tail would land in the page padding and be invisible.
    std::filesystem::resize_file(segs.front(), 3000);

    auto rdr = *w::SegmentReader::open(tmp.path());
    // We may successfully read some records before hitting the corruption.
    bool saw_error = false;
    while (true) {
        auto r = rdr.next();
        if (!r) {
            EXPECT_NE(r.error(), w::SegmentReadError::EndOfStream);
            saw_error = true;
            break;
        }
    }
    EXPECT_TRUE(saw_error);
}

TEST(SegmentReader, CrcCorruptionReported) {
    TempDir tmp;
    {
        auto wr = *w::SegmentWriter::open(tmp.path());
        ASSERT_TRUE(wr.log(bytes_of("hello")).has_value());
        ASSERT_TRUE(wr.cut().has_value());
    }
    // Flip a body byte to invalidate CRC.
    const auto seg = tmp.path() / "00000000";
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(seg, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    ASSERT_GT(bytes.size(), w::k_record_header_size);
    bytes[w::k_record_header_size] ^= 0x01;
    {
        std::ofstream out(seg, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    auto rdr = *w::SegmentReader::open(tmp.path());
    auto r = rdr.next();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), w::SegmentReadError::Crc);
}

TEST(SegmentReader, EndToEndWriterReaderFuzz) {
    TempDir tmp;
    std::mt19937_64 rng(0xDEFACED);
    std::vector<std::vector<std::uint8_t>> bodies;
    {
        auto wr = *w::SegmentWriter::open(tmp.path(), /*segment_size*/ 16 * 1024);
        for (int i = 0; i < 150; ++i) {
            const auto len = static_cast<std::size_t>(rng() % 4000);
            std::vector<std::uint8_t> b(len);
            for (auto& x : b) x = static_cast<std::uint8_t>(rng() & 0xFFU);
            bodies.push_back(b);
            ASSERT_TRUE(wr.log(b).has_value()) << "i=" << i << " len=" << len;
        }
        ASSERT_TRUE(wr.cut().has_value());
    }
    auto rdr = *w::SegmentReader::open(tmp.path());
    EXPECT_GT(rdr.segment_count(), 1u);
    EXPECT_EQ(drain(rdr), bodies);
}
