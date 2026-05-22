#include "merlion_tsdb/wal/page.hpp"

#include <algorithm>
#include <cassert>

#include "merlion_tsdb/encoding/crc32c.hpp"

namespace merlion_tsdb::wal {

// --- PageWriter -------------------------------------------------------------

std::size_t PageWriter::page_room_for_header() const noexcept {
    const auto pos = page_offset();
    if (k_page_size - pos < k_record_header_size) return 0;
    return k_page_size - pos - k_record_header_size;
}

void PageWriter::write_header(RecType t, std::uint16_t length, std::uint32_t crc) {
    buf_.push_back(static_cast<std::uint8_t>(t));
    // length: big-endian u16
    buf_.push_back(static_cast<std::uint8_t>(length >> 8));
    buf_.push_back(static_cast<std::uint8_t>(length));
    // crc: big-endian u32
    buf_.push_back(static_cast<std::uint8_t>(crc >> 24));
    buf_.push_back(static_cast<std::uint8_t>(crc >> 16));
    buf_.push_back(static_cast<std::uint8_t>(crc >> 8));
    buf_.push_back(static_cast<std::uint8_t>(crc));
}

void PageWriter::log(std::span<const std::uint8_t> body) {
    std::size_t pos = 0;
    const bool body_empty = body.empty();

    while (pos < body.size() || (body_empty && pos == 0)) {
        // If fewer than 7 bytes remain in the current page, zero-pad it. The
        // leading zero byte at the next read becomes a PageTerm.
        if (k_page_size - page_offset() < k_record_header_size) {
            const std::size_t pad = k_page_size - page_offset();
            buf_.insert(buf_.end(), pad, std::uint8_t{0});
        }

        const std::size_t page_room = page_room_for_header();
        const std::size_t remaining = body.size() - pos;
        const std::size_t fragment_len = std::min(remaining, page_room);

        const bool first_frag = (pos == 0);
        const bool last_frag  = (pos + fragment_len == body.size());

        RecType t;
        if (first_frag && last_frag) t = RecType::Full;
        else if (first_frag)         t = RecType::First;
        else if (last_frag)          t = RecType::Last;
        else                         t = RecType::Middle;

        const auto fragment = body.subspan(pos, fragment_len);
        const auto crc = crc32c::compute(fragment);
        write_header(t, static_cast<std::uint16_t>(fragment_len), crc);
        buf_.insert(buf_.end(), fragment.begin(), fragment.end());

        pos += fragment_len;
        if (body_empty) break;
    }
}

void PageWriter::close_page() {
    const auto pos = page_offset();
    if (pos == 0) return;  // already aligned
    buf_.insert(buf_.end(), k_page_size - pos, std::uint8_t{0});
}

// --- PageReader -------------------------------------------------------------

namespace {

std::uint16_t read_be_u16(std::span<const std::uint8_t> b) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(b[0]) << 8) | static_cast<std::uint16_t>(b[1]));
}

std::uint32_t read_be_u32(std::span<const std::uint8_t> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24) |
           (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8)  |
            static_cast<std::uint32_t>(b[3]);
}

}  // namespace

std::expected<void, WalReadError> PageReader::read_fragment_header(
    RecType& out_type, std::uint16_t& out_len, std::uint32_t& out_crc) noexcept {
    if (offset_ + k_record_header_size > buf_.size()) {
        return std::unexpected(WalReadError::UnexpectedEnd);
    }
    const auto h = buf_.subspan(offset_, k_record_header_size);
    const std::uint8_t type_byte = h[0];

    if ((type_byte & (k_snappy_mask | k_zstd_mask)) != 0) {
        return std::unexpected(WalReadError::UnsupportedCompression);
    }

    const std::uint8_t raw_type = type_byte & k_rec_type_mask;
    if (raw_type > static_cast<std::uint8_t>(RecType::Last)) {
        return std::unexpected(WalReadError::InvalidRecType);
    }
    out_type = static_cast<RecType>(raw_type);
    out_len  = read_be_u16(h.subspan(1, 2));
    out_crc  = read_be_u32(h.subspan(3, 4));
    offset_ += k_record_header_size;
    return {};
}

std::expected<std::span<const std::uint8_t>, WalReadError> PageReader::next() {
    record_.clear();
    bool open = false;  // a multi-fragment record is being assembled

    while (true) {
        if (offset_ >= buf_.size()) {
            if (open) return std::unexpected(WalReadError::TornRecord);
            return std::unexpected(WalReadError::EndOfStream);
        }

        // PageTerm: the rest of this page is zero padding. Skip to next page.
        if (buf_[offset_] == 0) {
            const std::size_t page_remaining = k_page_size - (offset_ % k_page_size);
            // If the padding tail isn't fully zero, that's corruption.
            const auto pad = buf_.subspan(offset_,
                std::min(page_remaining, buf_.size() - offset_));
            if (std::any_of(pad.begin(), pad.end(),
                            [](std::uint8_t b) { return b != 0; })) {
                return std::unexpected(WalReadError::InvalidRecType);
            }
            offset_ += pad.size();
            continue;
        }

        RecType type;
        std::uint16_t len;
        std::uint32_t crc;
        if (auto r = read_fragment_header(type, len, crc); !r) {
            return std::unexpected(r.error());
        }
        if (offset_ + len > buf_.size()) {
            return std::unexpected(WalReadError::UnexpectedEnd);
        }
        const auto fragment = buf_.subspan(offset_, len);
        offset_ += len;

        if (crc32c::compute(fragment) != crc) {
            return std::unexpected(WalReadError::CrcMismatch);
        }

        switch (type) {
            case RecType::Full:
                if (open) return std::unexpected(WalReadError::UnexpectedFirst);
                record_.assign(fragment.begin(), fragment.end());
                return std::span<const std::uint8_t>{record_};
            case RecType::First:
                if (open) return std::unexpected(WalReadError::UnexpectedFirst);
                record_.assign(fragment.begin(), fragment.end());
                open = true;
                break;
            case RecType::Middle:
                if (!open) return std::unexpected(WalReadError::UnexpectedMiddle);
                record_.insert(record_.end(), fragment.begin(), fragment.end());
                break;
            case RecType::Last:
                if (!open) return std::unexpected(WalReadError::UnexpectedMiddle);
                record_.insert(record_.end(), fragment.begin(), fragment.end());
                return std::span<const std::uint8_t>{record_};
            case RecType::PageTerm:
                // unreachable: handled by the zero-byte branch above.
                return std::unexpected(WalReadError::InvalidRecType);
        }
    }
}

}  // namespace merlion_tsdb::wal
