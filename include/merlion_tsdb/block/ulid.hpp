#pragma once

#include <array>
#include <cstdint>
#include <string>

// 26-character Crockford base32 ULID, byte-compatible with
// `github.com/oklog/ulid/v2`. The first 6 bytes encode a 48-bit
// big-endian millisecond timestamp; the trailing 10 bytes are random.
//
// Used as the directory name for each persistent block.
namespace merlion_tsdb::block {

// Crockford base32 alphabet — no I, L, O, or U to avoid visual ambiguity.
inline constexpr const char* k_crockford_alphabet =
    "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

inline constexpr std::size_t k_ulid_byte_size = 16;
inline constexpr std::size_t k_ulid_char_size = 26;

// Encodes a 16-byte ULID to its 26-character Crockford-base32 form.
// The 128 bits are emitted MSB-first; the encoder prepends two zero
// bits so the bit count rounds up to 26 × 5 = 130. As a result the
// first output character is in the range '0'..'7'.
[[nodiscard]] std::string encode_ulid(std::array<std::uint8_t, k_ulid_byte_size> bytes);

// Generates a new ULID: 48-bit ms-since-epoch timestamp + 80 bits from
// `std::random_device`. Suitable for block directory names — unique
// enough for any realistic single-host scenario, and lex-sortable by
// creation time.
[[nodiscard]] std::string new_ulid();

}  // namespace merlion_tsdb::block
