#include "merlion_tsdb/encoding/bstream.hpp"
#include "merlion_tsdb/encoding/varint.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace e = merlion_tsdb::encoding;

TEST(BitWriter, SingleBitsPackMsbFirst) {
    e::BitWriter w;
    // 1011_0010 -> 0xB2
    for (bool b : {true, false, true, true, false, false, true, false}) {
        w.write_bit(b);
    }
    auto bytes = w.bytes();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0xB2);
}

TEST(BitWriter, WriteByteAcrossPartialBoundary) {
    e::BitWriter w;
    w.write_bit(true);     // last byte now '1_______' (1 bit used, 7 free)
    w.write_byte(0xAB);    // 0xAB = 1010_1011; splits as 1010_101 | 1_______
    // First byte = '1' || top 7 bits of 0xAB = 1010_101 -> 1_1010101 = 0xD5
    // Second byte = bottom 1 bit of 0xAB shifted left by 7 -> 1_0000000 = 0x80
    auto bytes = w.bytes();
    ASSERT_EQ(bytes.size(), 2u);
    EXPECT_EQ(bytes[0], 0xD5);
    EXPECT_EQ(bytes[1], 0x80);
}

TEST(BitWriter, WriteBitsZeroNbitsIsNoop) {
    e::BitWriter w;
    w.write_bits(0xFFFF, 0);
    EXPECT_TRUE(w.bytes().empty());
}

TEST(BitWriterReader, RoundtripSingleBits) {
    e::BitWriter w;
    std::mt19937_64 rng(0xC0FFEE);
    std::vector<bool> bits;
    for (int i = 0; i < 257; ++i) {
        bool b = (rng() & 1U) != 0;
        bits.push_back(b);
        w.write_bit(b);
    }
    e::BitReader r(w.bytes());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        auto got = r.read_bit();
        ASSERT_TRUE(got.has_value()) << "at " << i;
        EXPECT_EQ(*got, bits[i]) << "at " << i;
    }
}

TEST(BitWriterReader, RoundtripVariableLengthBits) {
    e::BitWriter w;
    std::mt19937_64 rng(0xBEEF);
    std::vector<std::pair<std::uint64_t, int>> writes;
    for (int i = 0; i < 200; ++i) {
        int nbits = static_cast<int>(rng() % 65);
        std::uint64_t v = nbits == 64 ? rng()
                                      : (rng() & ((std::uint64_t{1} << nbits) - 1));
        writes.emplace_back(v, nbits);
        w.write_bits(v, nbits);
    }
    e::BitReader r(w.bytes());
    for (const auto& [v, nbits] : writes) {
        auto got = r.read_bits(static_cast<std::uint8_t>(nbits));
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, v) << "nbits=" << nbits;
    }
}

TEST(BitReader, ReadBitsSpansBufferBoundary) {
    // Write a long stream that forces the reader's 8-byte buffer to refill
    // mid-read.
    e::BitWriter w;
    for (int i = 0; i < 20; ++i) {
        w.write_bits(0xDEADBEEFCAFEBABEULL, 64);
    }
    e::BitReader r(w.bytes());
    for (int i = 0; i < 20; ++i) {
        auto v = r.read_bits(64);
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, 0xDEADBEEFCAFEBABEULL);
    }
}

TEST(BitReader, EOFReturnsEndOfStream) {
    e::BitWriter w;
    w.write_bits(0xFF, 8);
    e::BitReader r(w.bytes());
    EXPECT_TRUE(r.read_bits(8).has_value());
    EXPECT_EQ(r.read_bit().error(), e::ReadError::EndOfStream);
}

TEST(BitReader, ReadUvarintRoundtrip) {
    e::BitWriter w;
    // Write 7 (1 byte), 300 (2 bytes), and uint64 max (10 bytes) as raw bytes.
    std::array<std::uint8_t, merlion_tsdb::varint::max_varint_len64> buf{};
    auto n = merlion_tsdb::varint::put_uvarint(buf, 7);
    for (std::size_t i = 0; i < n; ++i) w.write_byte(buf[i]);
    n = merlion_tsdb::varint::put_uvarint(buf, 300);
    for (std::size_t i = 0; i < n; ++i) w.write_byte(buf[i]);
    n = merlion_tsdb::varint::put_uvarint(buf, std::numeric_limits<std::uint64_t>::max());
    for (std::size_t i = 0; i < n; ++i) w.write_byte(buf[i]);

    e::BitReader r(w.bytes());
    EXPECT_EQ(*r.read_uvarint(), 7u);
    EXPECT_EQ(*r.read_uvarint(), 300u);
    EXPECT_EQ(*r.read_uvarint(), std::numeric_limits<std::uint64_t>::max());
}

TEST(BitReader, ReadVarintRoundtrip) {
    e::BitWriter w;
    std::array<std::uint8_t, merlion_tsdb::varint::max_varint_len64> buf{};
    const std::array<std::int64_t, 5> vals{0, -1, 1, -42, 1'000'000};
    for (std::int64_t v : vals) {
        auto n = merlion_tsdb::varint::put_varint(buf, v);
        for (std::size_t i = 0; i < n; ++i) w.write_byte(buf[i]);
    }
    e::BitReader r(w.bytes());
    for (std::int64_t v : vals) {
        auto got = r.read_varint();
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, v);
    }
}

TEST(BitWriter, WriteBitsPreservesByteWriteSemantics) {
    // writeBits(u, 16) on an empty stream should produce the same bytes as
    // two write_byte calls because the bits are already byte-aligned.
    e::BitWriter a, b;
    a.write_bits(0xBEEF, 16);
    b.write_byte(0xBE);
    b.write_byte(0xEF);
    EXPECT_EQ(std::vector<std::uint8_t>(a.bytes().begin(), a.bytes().end()),
              std::vector<std::uint8_t>(b.bytes().begin(), b.bytes().end()));
}
