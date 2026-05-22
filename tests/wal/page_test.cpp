#include "merlion_tsdb/wal/page.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/encoding/crc32c.hpp"

namespace w = merlion_tsdb::wal;
namespace c = merlion_tsdb::crc32c;

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()),
            reinterpret_cast<const std::uint8_t*>(s.data() + s.size())};
}

std::vector<std::uint8_t> roundtrip_one(std::span<const std::uint8_t> body) {
    w::PageWriter writer;
    writer.log(body);
    w::PageReader reader(writer.bytes());
    auto r = reader.next();
    EXPECT_TRUE(r.has_value());
    return r ? std::vector<std::uint8_t>(r->begin(), r->end())
             : std::vector<std::uint8_t>{};
}

}  // namespace

TEST(PageWriter, SingleSmallRecordIsFullFragment) {
    auto body = bytes_of("hello");
    w::PageWriter writer;
    writer.log(body);
    const auto out = writer.bytes();
    ASSERT_EQ(out.size(), w::k_record_header_size + body.size());
    // Type byte: Full = 1
    EXPECT_EQ(out[0], 0x01);
    // Length BE u16 = 5
    EXPECT_EQ(out[1], 0x00);
    EXPECT_EQ(out[2], 0x05);
    // CRC matches Castagnoli over body.
    const std::uint32_t crc = c::compute(body);
    EXPECT_EQ((static_cast<std::uint32_t>(out[3]) << 24) |
              (static_cast<std::uint32_t>(out[4]) << 16) |
              (static_cast<std::uint32_t>(out[5]) << 8)  |
               static_cast<std::uint32_t>(out[6]),
              crc);
    // Body bytes follow header.
    for (std::size_t i = 0; i < body.size(); ++i) {
        EXPECT_EQ(out[w::k_record_header_size + i], body[i]);
    }
}

TEST(PageRoundtrip, EmptyRecord) {
    const std::vector<std::uint8_t> empty;
    EXPECT_EQ(roundtrip_one(empty), empty);
}

TEST(PageRoundtrip, ShortRecord) {
    auto body = bytes_of("hello world");
    EXPECT_EQ(roundtrip_one(body), body);
}

TEST(PageRoundtrip, ExactlyOneFullPageBody) {
    // Body of (page_size - header) → fills the page exactly, single Full fragment.
    std::vector<std::uint8_t> body(w::k_page_size - w::k_record_header_size,
                                   std::uint8_t{0xAB});
    EXPECT_EQ(roundtrip_one(body), body);
}

TEST(PageRoundtrip, BodyExceedsOnePageByOneByte) {
    std::vector<std::uint8_t> body(w::k_page_size - w::k_record_header_size + 1,
                                   std::uint8_t{0xCD});
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::uint8_t>(i);
    }
    auto got = roundtrip_one(body);
    EXPECT_EQ(got, body);
}

TEST(PageRoundtrip, BodySpansThreePages) {
    // Forces First + Middle + Last fragmenting.
    std::vector<std::uint8_t> body(2 * w::k_page_size + 5000);
    std::mt19937_64 rng(0xC0FFEE);
    for (auto& b : body) b = static_cast<std::uint8_t>(rng() & 0xFFU);
    auto got = roundtrip_one(body);
    EXPECT_EQ(got, body);
}

TEST(PageRoundtrip, ManyRecordsAreReturnedInOrder) {
    std::vector<std::vector<std::uint8_t>> bodies;
    bodies.push_back(bytes_of("first"));
    bodies.push_back(bytes_of("second"));
    bodies.push_back({0xDE, 0xAD, 0xBE, 0xEF});
    bodies.push_back({});  // empty record
    bodies.push_back(std::vector<std::uint8_t>(100, 0x42));
    bodies.push_back(std::vector<std::uint8_t>(w::k_page_size + 1000, 0x99));

    w::PageWriter writer;
    for (const auto& b : bodies) writer.log(b);
    w::PageReader reader(writer.bytes());

    for (std::size_t i = 0; i < bodies.size(); ++i) {
        auto got = reader.next();
        ASSERT_TRUE(got.has_value()) << "i=" << i;
        EXPECT_EQ(std::vector<std::uint8_t>(got->begin(), got->end()), bodies[i])
            << "i=" << i;
    }
    auto end = reader.next();
    ASSERT_FALSE(end.has_value());
    EXPECT_EQ(end.error(), w::WalReadError::EndOfStream);
}

TEST(PageRoundtrip, ClosePageZeroPadsToNextBoundary) {
    w::PageWriter writer;
    writer.log(bytes_of("small"));
    writer.close_page();
    // Buffer should be exactly one page long.
    EXPECT_EQ(writer.bytes().size(), w::k_page_size);
    // Tail bytes after the record should be zero.
    const auto body_end = w::k_record_header_size + 5;
    for (std::size_t i = body_end; i < w::k_page_size; ++i) {
        EXPECT_EQ(writer.bytes()[i], 0) << "i=" << i;
    }
    // Reader should still recover the record then cleanly hit EOF after the
    // padded page (PageTerm sentinel + zero tail).
    w::PageReader reader(writer.bytes());
    auto first = reader.next();
    ASSERT_TRUE(first.has_value());
    auto next = reader.next();
    ASSERT_FALSE(next.has_value());
    EXPECT_EQ(next.error(), w::WalReadError::EndOfStream);
}

TEST(PageReader, CrcMismatchDetected) {
    w::PageWriter writer;
    writer.log(bytes_of("payload"));
    auto buf = std::vector<std::uint8_t>(writer.bytes().begin(),
                                         writer.bytes().end());
    // Corrupt one byte of the body.
    buf[w::k_record_header_size] ^= 0x01;
    w::PageReader reader(buf);
    auto r = reader.next();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), w::WalReadError::CrcMismatch);
}

TEST(PageReader, TruncatedBodyIsUnexpectedEnd) {
    w::PageWriter writer;
    writer.log(bytes_of("payload"));
    auto buf = std::vector<std::uint8_t>(writer.bytes().begin(),
                                         writer.bytes().end());
    buf.pop_back();  // chop off one byte of the body
    w::PageReader reader(buf);
    auto r = reader.next();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), w::WalReadError::UnexpectedEnd);
}

TEST(PageReader, TornRecordAtEOF) {
    // Build a stream where the LAST page is missing the Last fragment of a
    // multi-page record (simulating a crash mid-write).
    std::vector<std::uint8_t> body(w::k_page_size + 1000, 0xAA);
    w::PageWriter writer;
    writer.log(body);
    auto buf = std::vector<std::uint8_t>(writer.bytes().begin(),
                                         writer.bytes().end());
    // Truncate: keep only the first page (which holds the First fragment).
    buf.resize(w::k_page_size);
    w::PageReader reader(buf);
    auto r = reader.next();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), w::WalReadError::TornRecord);
}

TEST(PageReader, RejectsCompressionFlags) {
    // Forge a fragment with the Snappy bit set; the MVP reader doesn't
    // support compression and must report it cleanly.
    std::vector<std::uint8_t> buf{
        // header
        static_cast<std::uint8_t>(0x01 | w::k_snappy_mask),  // Full + Snappy
        0x00, 0x05,  // length 5
        0x00, 0x00, 0x00, 0x00,  // bogus crc (won't be checked, type rejected first)
        // body
        'h', 'e', 'l', 'l', 'o'
    };
    w::PageReader reader(buf);
    auto r = reader.next();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), w::WalReadError::UnsupportedCompression);
}

TEST(PageWriter, ExactPageBoundaryRoundtripsAcrossManyPages) {
    // Stress: many records of random sizes, some smaller than one page,
    // some larger, randomly interleaved. Verifies that the writer's page
    // bookkeeping stays consistent across thousands of fragments.
    std::mt19937_64 rng(0xBADC0FFEE0DDF00DULL);
    std::vector<std::vector<std::uint8_t>> bodies;
    for (int i = 0; i < 200; ++i) {
        const auto pick = rng() % 7;
        std::size_t len;
        switch (pick) {
            case 0: len = 0; break;
            case 1: len = 1; break;
            case 2: len = static_cast<std::size_t>(rng() % 32); break;
            case 3: len = w::k_page_size - w::k_record_header_size - 1; break;
            case 4: len = w::k_page_size - w::k_record_header_size; break;
            case 5: len = w::k_page_size - w::k_record_header_size + 1; break;
            default: len = static_cast<std::size_t>(rng() % (4 * w::k_page_size));
        }
        std::vector<std::uint8_t> b(len);
        for (auto& x : b) x = static_cast<std::uint8_t>(rng() & 0xFFU);
        bodies.push_back(std::move(b));
    }

    w::PageWriter writer;
    for (const auto& b : bodies) writer.log(b);
    w::PageReader reader(writer.bytes());
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        auto got = reader.next();
        ASSERT_TRUE(got.has_value()) << "i=" << i << " len=" << bodies[i].size();
        EXPECT_EQ(std::vector<std::uint8_t>(got->begin(), got->end()), bodies[i])
            << "i=" << i << " len=" << bodies[i].size();
    }
}
