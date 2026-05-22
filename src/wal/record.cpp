#include "merlion_tsdb/wal/record.hpp"

#include <array>
#include <bit>
#include <cstring>

#include "merlion_tsdb/encoding/varint.hpp"

namespace merlion_tsdb::wal::record {

namespace {

// ST marker byte values (must match Go's `noST/sameST/explicitST` iota at
// record.go:967).
constexpr std::uint8_t k_st_no       = 0;
constexpr std::uint8_t k_st_same     = 1;
constexpr std::uint8_t k_st_explicit = 2;

// --- Encoder helpers --------------------------------------------------------

void append_uvarint(std::vector<std::uint8_t>& buf, std::uint64_t x) {
    std::array<std::uint8_t, varint::max_varint_len64> tmp{};
    const auto n = varint::put_uvarint(tmp, x);
    buf.insert(buf.end(), tmp.begin(), tmp.begin() + n);
}

void append_varint(std::vector<std::uint8_t>& buf, std::int64_t x) {
    std::array<std::uint8_t, varint::max_varint_len64> tmp{};
    const auto n = varint::put_varint(tmp, x);
    buf.insert(buf.end(), tmp.begin(), tmp.begin() + n);
}

void append_be64(std::vector<std::uint8_t>& buf, std::uint64_t x) {
    buf.push_back(static_cast<std::uint8_t>(x >> 56));
    buf.push_back(static_cast<std::uint8_t>(x >> 48));
    buf.push_back(static_cast<std::uint8_t>(x >> 40));
    buf.push_back(static_cast<std::uint8_t>(x >> 32));
    buf.push_back(static_cast<std::uint8_t>(x >> 24));
    buf.push_back(static_cast<std::uint8_t>(x >> 16));
    buf.push_back(static_cast<std::uint8_t>(x >> 8));
    buf.push_back(static_cast<std::uint8_t>(x));
}

void append_uvarint_str(std::vector<std::uint8_t>& buf, std::string_view s) {
    append_uvarint(buf, s.size());
    buf.insert(buf.end(),
               reinterpret_cast<const std::uint8_t*>(s.data()),
               reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
}

// --- Decoder ---------------------------------------------------------------

class Decoder {
public:
    explicit Decoder(std::span<const std::uint8_t> b) noexcept : buf_(b) {}

    [[nodiscard]] bool has_more() const noexcept {
        return !err_ && offset_ < buf_.size();
    }
    [[nodiscard]] bool error() const noexcept { return err_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return offset_ <= buf_.size() ? buf_.size() - offset_ : 0;
    }

    std::uint8_t read_byte() noexcept {
        if (offset_ >= buf_.size()) { err_ = true; return 0; }
        return buf_[offset_++];
    }

    std::uint64_t read_be64() noexcept {
        if (offset_ + 8 > buf_.size()) { err_ = true; return 0; }
        std::uint64_t v =
            (static_cast<std::uint64_t>(buf_[offset_]    ) << 56) |
            (static_cast<std::uint64_t>(buf_[offset_ + 1]) << 48) |
            (static_cast<std::uint64_t>(buf_[offset_ + 2]) << 40) |
            (static_cast<std::uint64_t>(buf_[offset_ + 3]) << 32) |
            (static_cast<std::uint64_t>(buf_[offset_ + 4]) << 24) |
            (static_cast<std::uint64_t>(buf_[offset_ + 5]) << 16) |
            (static_cast<std::uint64_t>(buf_[offset_ + 6]) << 8)  |
             static_cast<std::uint64_t>(buf_[offset_ + 7]);
        offset_ += 8;
        return v;
    }

    std::int64_t read_be64_int64() noexcept {
        return static_cast<std::int64_t>(read_be64());
    }

    std::uint64_t read_uvarint() noexcept {
        auto r = varint::read_uvarint(buf_.subspan(offset_));
        if (!r) { err_ = true; return 0; }
        offset_ += r->second;
        return r->first;
    }

    std::int64_t read_varint() noexcept {
        auto r = varint::read_varint(buf_.subspan(offset_));
        if (!r) { err_ = true; return 0; }
        offset_ += r->second;
        return r->first;
    }

    std::string_view read_uvarint_str() noexcept {
        const auto len = read_uvarint();
        if (err_) return {};
        if (offset_ + len > buf_.size()) { err_ = true; return {}; }
        auto s = std::string_view(
            reinterpret_cast<const char*>(buf_.data() + offset_),
            static_cast<std::size_t>(len));
        offset_ += len;
        return s;
    }

private:
    std::span<const std::uint8_t> buf_;
    std::size_t offset_ = 0;
    bool err_ = false;
};

}  // namespace

// --- Series -----------------------------------------------------------------

std::vector<std::uint8_t> encode_series(std::span<const RefSeries> series) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(Type::Series));
    for (const auto& s : series) {
        append_be64(out, s.ref);
        append_uvarint(out, s.labels.size());
        for (const auto& l : s.labels) {
            append_uvarint_str(out, l.name);
            append_uvarint_str(out, l.value);
        }
    }
    return out;
}

std::expected<std::vector<RefSeries>, RecordError>
decode_series(std::span<const std::uint8_t> rec) {
    Decoder d(rec);
    if (d.read_byte() != static_cast<std::uint8_t>(Type::Series)) {
        return std::unexpected(RecordError::InvalidType);
    }

    std::vector<RefSeries> out;
    while (d.has_more()) {
        RefSeries s;
        s.ref = d.read_be64();
        const auto label_count = d.read_uvarint();
        s.labels.reserve(static_cast<std::size_t>(label_count));
        for (std::uint64_t i = 0; i < label_count; ++i) {
            const auto name  = d.read_uvarint_str();
            const auto value = d.read_uvarint_str();
            if (d.error()) return std::unexpected(RecordError::UnexpectedEnd);
            s.labels.push_back({std::string(name), std::string(value)});
        }
        if (d.error()) return std::unexpected(RecordError::UnexpectedEnd);
        out.push_back(std::move(s));
    }
    if (d.error()) return std::unexpected(RecordError::UnexpectedEnd);
    if (d.remaining() != 0) return std::unexpected(RecordError::TrailingBytes);
    return out;
}

// --- SamplesV2 --------------------------------------------------------------

namespace {

void write_st_marker(std::vector<std::uint8_t>& out,
                     std::int64_t st, std::int64_t first_st,
                     std::int64_t prev_st) {
    if (st == 0) {
        out.push_back(k_st_no);
    } else if (st == prev_st) {
        out.push_back(k_st_same);
    } else {
        out.push_back(k_st_explicit);
        append_varint(out, st - first_st);
    }
}

}  // namespace

std::vector<std::uint8_t>
encode_samples_v2(std::span<const RefSample> samples) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(Type::SamplesV2));
    if (samples.empty()) return out;

    // First sample carries the absolute timestamps; subsequent samples are
    // deltas. Both ref and t deltas are signed.
    const auto& first = samples.front();
    append_varint(out, static_cast<std::int64_t>(first.ref));
    append_varint(out, first.t);
    append_varint(out, first.st);
    append_be64(out, std::bit_cast<std::uint64_t>(first.v));

    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto& s    = samples[i];
        const auto& prev = samples[i - 1];

        append_varint(out, static_cast<std::int64_t>(s.ref) -
                          static_cast<std::int64_t>(prev.ref));
        append_varint(out, s.t - first.t);
        write_st_marker(out, s.st, first.st, prev.st);
        append_be64(out, std::bit_cast<std::uint64_t>(s.v));
    }
    return out;
}

std::expected<std::vector<RefSample>, RecordError>
decode_samples_v2(std::span<const std::uint8_t> rec) {
    Decoder d(rec);
    if (d.read_byte() != static_cast<std::uint8_t>(Type::SamplesV2)) {
        return std::unexpected(RecordError::InvalidType);
    }
    std::vector<RefSample> out;
    if (!d.has_more()) {
        if (d.error()) return std::unexpected(RecordError::UnexpectedEnd);
        return out;  // empty record is legal
    }

    std::int64_t first_t = 0;
    std::int64_t first_st = 0;

    while (d.has_more()) {
        RefSample sample{};
        if (out.empty()) {
            sample.ref = static_cast<SeriesRef>(d.read_varint());
            first_t    = d.read_varint();
            sample.t   = first_t;
            sample.st  = d.read_varint();
            first_st   = sample.st;
        } else {
            const auto& prev = out.back();
            sample.ref = static_cast<SeriesRef>(
                static_cast<std::int64_t>(prev.ref) + d.read_varint());
            sample.t   = first_t + d.read_varint();

            const auto marker = d.read_byte();
            if (d.error()) return std::unexpected(RecordError::UnexpectedEnd);
            switch (marker) {
                case k_st_no:       sample.st = 0;            break;
                case k_st_same:     sample.st = prev.st;      break;
                case k_st_explicit: sample.st = first_st + d.read_varint(); break;
                default:
                    return std::unexpected(RecordError::StMarkerInvalid);
            }
        }
        sample.v = std::bit_cast<double>(d.read_be64());
        if (d.error()) return std::unexpected(RecordError::UnexpectedEnd);
        out.push_back(sample);
    }
    if (d.error())    return std::unexpected(RecordError::UnexpectedEnd);
    if (d.remaining() != 0) return std::unexpected(RecordError::TrailingBytes);
    return out;
}

}  // namespace merlion_tsdb::wal::record
