#include "merlion_tsdb/block/ulid.hpp"

#include <chrono>
#include <random>

namespace merlion_tsdb::block {

std::string encode_ulid(std::array<std::uint8_t, k_ulid_byte_size> bytes) {
    std::string out;
    out.resize(k_ulid_char_size);

    // Walk the 130-bit stream (2 leading zero bits + 128 ULID bits).
    // For each output char, accumulate 5 successive bits MSB-first.
    auto bit_at = [&](std::size_t virtual_pos) noexcept -> std::uint8_t {
        // Bit 0 and 1 are the synthetic leading zeros. Bit i (i >= 2) is
        // bit (i - 2) of the 128-bit ULID, big-endian: bytes[0] holds
        // bits 0..7, bytes[1] holds 8..15, etc.
        if (virtual_pos < 2) return 0;
        const auto real_pos = virtual_pos - 2;
        return static_cast<std::uint8_t>(
            (bytes[real_pos / 8] >> (7U - (real_pos % 8U))) & 1U);
    };

    for (std::size_t c = 0; c < k_ulid_char_size; ++c) {
        std::uint8_t v = 0;
        for (std::size_t b = 0; b < 5; ++b) {
            v = static_cast<std::uint8_t>((v << 1) | bit_at(c * 5 + b));
        }
        out[c] = k_crockford_alphabet[v];
    }
    return out;
}

std::string new_ulid() {
    // Timestamp: 48 bits ms-since-epoch, big-endian into bytes 0..5.
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto ts = static_cast<std::uint64_t>(now_ms) & 0x0000'FFFF'FFFF'FFFFULL;

    std::array<std::uint8_t, k_ulid_byte_size> bytes{};
    bytes[0] = static_cast<std::uint8_t>(ts >> 40);
    bytes[1] = static_cast<std::uint8_t>(ts >> 32);
    bytes[2] = static_cast<std::uint8_t>(ts >> 24);
    bytes[3] = static_cast<std::uint8_t>(ts >> 16);
    bytes[4] = static_cast<std::uint8_t>(ts >> 8);
    bytes[5] = static_cast<std::uint8_t>(ts);

    // Randomness: 80 bits into bytes 6..15. std::random_device returns
    // 32 bits per call; pull three times and fold in.
    std::random_device rd;
    for (std::size_t i = 6; i < k_ulid_byte_size; i += 4) {
        const auto r = rd();
        for (std::size_t j = 0; j < 4 && (i + j) < k_ulid_byte_size; ++j) {
            bytes[i + j] = static_cast<std::uint8_t>(r >> (8U * j));
        }
    }

    return encode_ulid(bytes);
}

}  // namespace merlion_tsdb::block
