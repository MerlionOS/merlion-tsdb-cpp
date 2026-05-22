#include "merlion_tsdb/encoding/crc32c.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace c = merlion_tsdb::crc32c;

namespace {

std::span<const std::uint8_t> as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace

// Test vectors cross-checked against Go's hash/crc32.Checksum with the
// Castagnoli polynomial. If any of these regress, the polynomial or table
// generation is wrong and the WAL framing will be incompatible with upstream.
TEST(Crc32c, EmptyInputIsZero) {
    EXPECT_EQ(c::compute({}), 0x00000000U);
}

TEST(Crc32c, CanonicalCheckValue) {
    // "123456789" — the universal CRC32C check value cited by ISO/IEC and RFC 3720.
    EXPECT_EQ(c::compute(as_bytes("123456789")), c::k_check);
    EXPECT_EQ(c::k_check, 0xE3069283U);
}

TEST(Crc32c, KnownShortStrings) {
    EXPECT_EQ(c::compute(as_bytes("a")),       0xC1D04330U);
    EXPECT_EQ(c::compute(as_bytes("abc")),     0x364B3FB7U);
    EXPECT_EQ(c::compute(as_bytes("hello")),   0x9A71BB4CU);
    // Cross-checked: in Go,
    //   crc32.Checksum([]byte("The quick brown fox jumps over the lazy dog"),
    //                  crc32.MakeTable(crc32.Castagnoli)) == 0x22620404
    EXPECT_EQ(c::compute(as_bytes("The quick brown fox jumps over the lazy dog")),
              0x22620404U);
}

TEST(Crc32c, StreamingMatchesOneShot) {
    // Splitting the input into two chunks must produce the same CRC as
    // hashing the whole thing in one go. The framing code splits records
    // across pages, so this is a load-bearing invariant.
    const auto data = as_bytes("The quick brown fox jumps over the lazy dog");
    const auto one_shot = c::compute(data);

    for (std::size_t split = 0; split <= data.size(); ++split) {
        const auto a = data.subspan(0, split);
        const auto b = data.subspan(split);
        const auto streamed = c::finalize(c::update(c::update(c::k_init, a), b));
        EXPECT_EQ(streamed, one_shot) << "split=" << split;
    }
}

TEST(Crc32c, StreamingFromMidwayThreeChunks) {
    std::mt19937_64 rng(0xABCDEF);
    std::vector<std::uint8_t> data(1024);
    for (auto& b : data) b = static_cast<std::uint8_t>(rng() & 0xFFU);

    const auto whole = c::compute(data);

    const std::span<const std::uint8_t> full{data};
    const auto p1 = full.subspan(0,   200);
    const auto p2 = full.subspan(200, 600);
    const auto p3 = full.subspan(800);

    auto state = c::k_init;
    state = c::update(state, p1);
    state = c::update(state, p2);
    state = c::update(state, p3);
    EXPECT_EQ(c::finalize(state), whole);
}

TEST(Crc32c, BinaryDataRoundtrip) {
    // 256 distinct byte values, common edge case.
    std::array<std::uint8_t, 256> all_bytes{};
    for (std::size_t i = 0; i < all_bytes.size(); ++i) {
        all_bytes[i] = static_cast<std::uint8_t>(i);
    }
    // Cross-checked against Go:
    //   crc32.Checksum(buf[:], crc32.MakeTable(crc32.Castagnoli)) == 0x9C44184B
    EXPECT_EQ(c::compute(all_bytes), 0x9C44184BU);
}
