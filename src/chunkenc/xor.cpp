#include "merlion_tsdb/chunkenc/xor.hpp"

#include <array>
#include <bit>

#include "merlion_tsdb/encoding/varint.hpp"

namespace merlion_tsdb::chunkenc {

namespace {

// True iff x fits in `nbits` of two's-complement, with Prometheus's
// asymmetric range:  -(2^(n-1) - 1) <= x <= 2^(n-1).
// Note the missing -1 on the lower bound vs textbook two's complement —
// this matches Go and SPEC.md §3.1.
constexpr bool bit_range(std::int64_t x, std::uint8_t n) noexcept {
    const std::int64_t hi = std::int64_t{1} << (n - 1);
    const std::int64_t lo = -(hi - 1);
    return lo <= x && x <= hi;
}

void put_uvarint_via_writer(encoding::BitWriter& w, std::uint64_t x) noexcept {
    std::array<std::uint8_t, varint::max_varint_len64> buf{};
    const auto n = varint::put_uvarint(buf, x);
    for (std::size_t i = 0; i < n; ++i) w.write_byte(buf[i]);
}

void put_varint_via_writer(encoding::BitWriter& w, std::int64_t x) noexcept {
    std::array<std::uint8_t, varint::max_varint_len64> buf{};
    const auto n = varint::put_varint(buf, x);
    for (std::size_t i = 0; i < n; ++i) w.write_byte(buf[i]);
}

// Reads the 14/17/20-bit dod payload and sign-extends manually because the
// width is not a multiple of 8 and the value was packed as unsigned bits.
std::int64_t sign_extend(std::uint64_t bits, std::uint8_t width) noexcept {
    if (bits > (std::uint64_t{1} << (width - 1))) {
        // The shift-by-width below is safe because width is one of 14, 17, 20.
        bits -= std::uint64_t{1} << width;
    }
    return static_cast<std::int64_t>(bits);
}

}  // namespace

// --- XORChunk ---------------------------------------------------------------

XORChunk::XORChunk() {
    // Allocate just the 2-byte header. write_byte / write_bits will grow
    // the buffer as samples are appended.
    std::vector<std::uint8_t> initial(kHeaderSize, std::uint8_t{0});
    writer_.reset(std::move(initial));
}

std::uint16_t XORChunk::num_samples() const noexcept {
    const auto b = writer_.bytes();
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(b[0]) << 8) | static_cast<std::uint16_t>(b[1]));
}

void XORChunk::compact() {
    // No-op for std::vector — capacity reclaim happens via shrink_to_fit if
    // the caller wants it. We avoid forcing a reallocation; matches Go's
    // chunkCompactCapacityThreshold-style cheapness more closely than a
    // mandatory copy.
    auto bytes = writer_.release();
    bytes.shrink_to_fit();
    writer_.reset(std::move(bytes));
}

std::expected<XORAppender, encoding::ReadError> XORChunk::appender() {
    if (writer_.bytes().size() == kHeaderSize) {
        return XORAppender{&writer_};
    }
    auto it = iterator();
    while (it.next()) {}
    if (auto err = it.error(); err) {
        return std::unexpected(*err);
    }
    return XORAppender{&writer_, it.t_, it.v_, it.t_delta_, it.leading_, it.trailing_};
}

XORIterator XORChunk::iterator() const noexcept {
    return XORIterator{writer_.bytes()};
}

// --- XORAppender ------------------------------------------------------------

bool XORAppender::append(std::int64_t t, double v) noexcept {
    auto buf = writer_->mutable_bytes();
    const std::uint16_t num = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(buf[0]) << 8) | static_cast<std::uint16_t>(buf[1]));

    if (num == XORChunk::kMaxSamples) {
        // Chunk full. Caller must cut a new chunk.
        return false;
    }

    std::uint64_t t_delta = 0;

    switch (num) {
        case 0: {
            // First sample: signed varint t, then 64 raw bits of v.
            put_varint_via_writer(*writer_, t);
            writer_->write_bits(std::bit_cast<std::uint64_t>(v), 64);
            break;
        }
        case 1: {
            t_delta = static_cast<std::uint64_t>(t - t_);
            put_uvarint_via_writer(*writer_, t_delta);
            write_v_delta(v);
            break;
        }
        default: {
            t_delta = static_cast<std::uint64_t>(t - t_);
            const std::int64_t dod = static_cast<std::int64_t>(t_delta - t_delta_);

            // Prefix-coded delta-of-delta. See SPEC.md §3.1.
            if (dod == 0) {
                writer_->write_bit(false);
            } else if (bit_range(dod, 14)) {
                // '10' (2 bits) + 14-bit dod packed as two byte writes.
                writer_->write_byte(static_cast<std::uint8_t>(
                    0x80U | (static_cast<std::uint8_t>(dod >> 8) & 0x3FU)));
                writer_->write_byte(static_cast<std::uint8_t>(dod));
            } else if (bit_range(dod, 17)) {
                writer_->write_bits(0b110U, 3);
                writer_->write_bits(static_cast<std::uint64_t>(dod), 17);
            } else if (bit_range(dod, 20)) {
                writer_->write_bits(0b1110U, 4);
                writer_->write_bits(static_cast<std::uint64_t>(dod), 20);
            } else {
                writer_->write_bits(0b1111U, 4);
                writer_->write_bits(static_cast<std::uint64_t>(dod), 64);
            }

            write_v_delta(v);
            break;
        }
    }

    t_ = t;
    v_ = v;
    t_delta_ = t_delta;

    // Re-fetch — writes may have invalidated the earlier span via reallocation.
    buf = writer_->mutable_bytes();
    const std::uint16_t new_num = static_cast<std::uint16_t>(num + 1U);
    buf[0] = static_cast<std::uint8_t>(new_num >> 8);
    buf[1] = static_cast<std::uint8_t>(new_num);
    return true;
}

void XORAppender::write_v_delta(double v) noexcept {
    xor_write(*writer_, v, v_, leading_, trailing_);
}

// --- XORIterator ------------------------------------------------------------

XORIterator::XORIterator(std::span<const std::uint8_t> bytes) noexcept
    : reader_(bytes.size() >= XORChunk::kHeaderSize
                  ? bytes.subspan(XORChunk::kHeaderSize)
                  : std::span<const std::uint8_t>{}) {
    if (bytes.size() >= XORChunk::kHeaderSize) {
        num_total_ = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[0]) << 8) |
            static_cast<std::uint16_t>(bytes[1]));
    }
}

bool XORIterator::next() noexcept {
    if (err_ || num_read_ == num_total_) return false;

    if (num_read_ == 0) {
        auto t = reader_.read_varint();
        if (!t) { err_ = t.error(); return false; }
        auto v = reader_.read_bits(64);
        if (!v) { err_ = v.error(); return false; }
        t_  = *t;
        v_  = std::bit_cast<double>(*v);
        ++num_read_;
        return true;
    }
    if (num_read_ == 1) {
        auto td = reader_.read_uvarint();
        if (!td) { err_ = td.error(); return false; }
        t_delta_ = *td;
        t_ += static_cast<std::int64_t>(t_delta_);
        return read_value();
    }

    // Decode the delta-of-delta prefix one bit at a time.
    std::uint8_t prefix = 0;
    for (int i = 0; i < 4; ++i) {
        auto bit = reader_.read_bit();
        if (!bit) { err_ = bit.error(); return false; }
        prefix = static_cast<std::uint8_t>(prefix << 1U);
        if (!*bit) break;
        prefix = static_cast<std::uint8_t>(prefix | 1U);
    }

    std::uint8_t  payload_width = 0;
    std::int64_t  dod = 0;
    switch (prefix) {
        case 0b0:    /* dod stays 0 */         break;
        case 0b10:   payload_width = 14;       break;
        case 0b110:  payload_width = 17;       break;
        case 0b1110: payload_width = 20;       break;
        case 0b1111: {
            auto bits = reader_.read_bits(64);
            if (!bits) { err_ = bits.error(); return false; }
            dod = static_cast<std::int64_t>(*bits);
            break;
        }
        default:
            // Unreachable: only 1-4 bits read into prefix above.
            return false;
    }

    if (payload_width != 0) {
        auto bits = reader_.read_bits(payload_width);
        if (!bits) { err_ = bits.error(); return false; }
        dod = sign_extend(*bits, payload_width);
    }

    t_delta_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(t_delta_) + dod);
    t_ += static_cast<std::int64_t>(t_delta_);
    return read_value();
}

bool XORIterator::read_value() noexcept {
    auto r = xor_read(reader_, v_, leading_, trailing_);
    if (!r) { err_ = r.error(); return false; }
    ++num_read_;
    return true;
}

// --- xor_write / xor_read ---------------------------------------------------

void xor_write(encoding::BitWriter& w, double new_v, double prev_v,
               std::uint8_t& leading, std::uint8_t& trailing) noexcept {
    const std::uint64_t delta =
        std::bit_cast<std::uint64_t>(new_v) ^ std::bit_cast<std::uint64_t>(prev_v);

    if (delta == 0) {
        w.write_bit(false);
        return;
    }
    w.write_bit(true);

    std::uint8_t new_leading  = static_cast<std::uint8_t>(std::countl_zero(delta));
    std::uint8_t new_trailing = static_cast<std::uint8_t>(std::countr_zero(delta));

    // Clamp leading to fit in the 5-bit field used for fresh-window encoding.
    if (new_leading >= 32) new_leading = 31;

    if (leading != 0xFFU && new_leading >= leading && new_trailing >= trailing) {
        // Reuse the existing (leading, trailing) window.
        w.write_bit(false);
        const std::uint8_t sigbits = static_cast<std::uint8_t>(64U - leading - trailing);
        w.write_bits(delta >> trailing, sigbits);
        return;
    }

    leading  = new_leading;
    trailing = new_trailing;

    w.write_bit(true);
    w.write_bits(new_leading, 5);

    // sigbits == 64 doesn't fit in 6 bits; encode as 0 and let the reader
    // promote it back. sigbits == 0 cannot occur — delta == 0 short-circuited.
    const std::uint8_t sigbits = static_cast<std::uint8_t>(64U - new_leading - new_trailing);
    w.write_bits(sigbits, 6);
    w.write_bits(delta >> new_trailing, sigbits);
}

std::expected<void, encoding::ReadError>
xor_read(encoding::BitReader& r, double& value,
         std::uint8_t& leading, std::uint8_t& trailing) noexcept {
    auto bit = r.read_bit();
    if (!bit) return std::unexpected(bit.error());
    if (!*bit) return {};  // delta == 0, value unchanged.

    bit = r.read_bit();
    if (!bit) return std::unexpected(bit.error());

    std::uint8_t mbits = 0;
    if (!*bit) {
        // Reuse previous window.
        mbits = static_cast<std::uint8_t>(64U - leading - trailing);
    } else {
        auto lz = r.read_bits(5);
        if (!lz) return std::unexpected(lz.error());
        leading = static_cast<std::uint8_t>(*lz);

        auto sb = r.read_bits(6);
        if (!sb) return std::unexpected(sb.error());
        mbits = static_cast<std::uint8_t>(*sb);
        if (mbits == 0) mbits = 64;  // see comment in xor_write.
        trailing = static_cast<std::uint8_t>(64U - leading - mbits);
    }

    auto bits = r.read_bits(mbits);
    if (!bits) return std::unexpected(bits.error());

    auto vbits = std::bit_cast<std::uint64_t>(value);
    vbits ^= (*bits) << trailing;
    value = std::bit_cast<double>(vbits);
    return {};
}

}  // namespace merlion_tsdb::chunkenc
