#include "merlion_tsdb/encoding/varint.hpp"

#include <array>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace m = merlion_tsdb;

namespace {

std::vector<std::uint8_t> encode_uvarint(std::uint64_t x) {
    std::array<std::uint8_t, m::varint::max_varint_len64> buf{};
    const auto n = m::varint::put_uvarint(buf, x);
    return {buf.begin(), buf.begin() + n};
}

std::vector<std::uint8_t> encode_varint(std::int64_t x) {
    std::array<std::uint8_t, m::varint::max_varint_len64> buf{};
    const auto n = m::varint::put_varint(buf, x);
    return {buf.begin(), buf.begin() + n};
}

}  // namespace

// Byte-level fixtures cross-checked against Go's encoding/binary.PutUvarint /
// PutVarint. Locking these prevents silent ABI drift.

TEST(Uvarint, KnownBytes) {
    EXPECT_EQ(encode_uvarint(0),    (std::vector<std::uint8_t>{0x00}));
    EXPECT_EQ(encode_uvarint(1),    (std::vector<std::uint8_t>{0x01}));
    EXPECT_EQ(encode_uvarint(127),  (std::vector<std::uint8_t>{0x7F}));
    EXPECT_EQ(encode_uvarint(128),  (std::vector<std::uint8_t>{0x80, 0x01}));
    EXPECT_EQ(encode_uvarint(300),  (std::vector<std::uint8_t>{0xAC, 0x02}));
    EXPECT_EQ(encode_uvarint(16384), (std::vector<std::uint8_t>{0x80, 0x80, 0x01}));
}

TEST(Uvarint, MaxUint64) {
    auto bytes = encode_uvarint(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(bytes.size(), 10u);
    // Reads back identically.
    auto r = m::varint::read_uvarint(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(r->second, 10u);
}

TEST(Varint, KnownBytes) {
    // Zigzag mapping: 0->0, -1->1, 1->2, -2->3, 2->4, ...
    EXPECT_EQ(encode_varint(0),  (std::vector<std::uint8_t>{0x00}));
    EXPECT_EQ(encode_varint(-1), (std::vector<std::uint8_t>{0x01}));
    EXPECT_EQ(encode_varint(1),  (std::vector<std::uint8_t>{0x02}));
    EXPECT_EQ(encode_varint(-2), (std::vector<std::uint8_t>{0x03}));
    EXPECT_EQ(encode_varint(63), (std::vector<std::uint8_t>{0x7E}));
    EXPECT_EQ(encode_varint(64), (std::vector<std::uint8_t>{0x80, 0x01}));
    EXPECT_EQ(encode_varint(-64),(std::vector<std::uint8_t>{0x7F}));
}

TEST(Varint, Roundtrip) {
    for (std::int64_t v : std::initializer_list<std::int64_t>{
             std::numeric_limits<std::int64_t>::min(),
             -1'000'000, -42, -1, 0, 1, 42, 1'000'000,
             std::numeric_limits<std::int64_t>::max()}) {
        const auto bytes = encode_varint(v);
        auto r = m::varint::read_varint(bytes);
        ASSERT_TRUE(r.has_value()) << "value=" << v;
        EXPECT_EQ(r->first, v);
        EXPECT_EQ(r->second, bytes.size());
    }
}

TEST(Varint, EmptyBufferIsEOF) {
    std::vector<std::uint8_t> empty;
    auto r = m::varint::read_uvarint(empty);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), m::encoding::ReadError::EndOfStream);
}

TEST(Varint, TruncatedBufferIsUnexpectedEnd) {
    // Continuation bit set but stream ends.
    std::vector<std::uint8_t> truncated{0x80, 0x80};
    auto r = m::varint::read_uvarint(truncated);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), m::encoding::ReadError::UnexpectedEnd);
}

TEST(Varint, OverlongOverflowsAtTenthByte) {
    // Eleven continuation bytes is overflow territory.
    std::vector<std::uint8_t> overlong(11, 0x80);
    overlong.back() = 0x01;
    auto r = m::varint::read_uvarint(overlong);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), m::encoding::ReadError::VarintOverflow);
}
