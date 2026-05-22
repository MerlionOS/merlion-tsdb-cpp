#include "merlion_tsdb/wal/segment_reader.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>

namespace merlion_tsdb::wal {

namespace {

constexpr std::size_t k_segment_name_width = 8;

std::expected<std::size_t, std::errc>
parse_segment_name(std::string_view name) noexcept {
    if (name.size() != k_segment_name_width) {
        return std::unexpected(std::errc::invalid_argument);
    }
    std::size_t v = 0;
    auto r = std::from_chars(name.data(), name.data() + name.size(), v);
    if (r.ec != std::errc{} || r.ptr != name.data() + name.size()) {
        return std::unexpected(std::errc::invalid_argument);
    }
    return v;
}

SegmentReadError map_page_error(WalReadError e) noexcept {
    switch (e) {
        case WalReadError::EndOfStream:
        case WalReadError::TornRecord:
            // Caller decides whether torn-at-tail is fatal.
            return SegmentReadError::EndOfStream;
        case WalReadError::CrcMismatch:
            return SegmentReadError::Crc;
        case WalReadError::UnsupportedCompression:
            return SegmentReadError::UnsupportedCompression;
        case WalReadError::UnexpectedEnd:
        case WalReadError::InvalidRecType:
        case WalReadError::UnexpectedFirst:
        case WalReadError::UnexpectedMiddle:
            return SegmentReadError::InvalidFraming;
    }
    return SegmentReadError::InvalidFraming;
}

}  // namespace

std::expected<SegmentReader, std::error_code>
SegmentReader::open(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (ec) return std::unexpected(ec);
        return std::unexpected(std::make_error_code(std::errc::not_a_directory));
    }

    std::vector<std::pair<std::size_t, std::filesystem::path>> indexed;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return std::unexpected(ec);
        if (!entry.is_regular_file()) continue;
        auto parsed = parse_segment_name(entry.path().filename().string());
        if (!parsed) continue;
        indexed.emplace_back(*parsed, entry.path());
    }
    if (ec) return std::unexpected(ec);

    std::sort(indexed.begin(), indexed.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::filesystem::path> paths;
    paths.reserve(indexed.size());
    for (auto& [_, p] : indexed) paths.push_back(std::move(p));

    return SegmentReader{std::move(paths)};
}

std::filesystem::path SegmentReader::current_segment_path() const {
    if (current_index_ >= segment_paths_.size()) return {};
    return segment_paths_[current_index_];
}

std::expected<void, SegmentReadError> SegmentReader::load_current_segment() {
    std::ifstream in(segment_paths_[current_index_], std::ios::binary);
    if (!in) return std::unexpected(SegmentReadError::SegmentOpen);

    current_bytes_.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>{});
    if (in.bad()) return std::unexpected(SegmentReadError::SegmentOpen);

    page_reader_.emplace(std::span<const std::uint8_t>{current_bytes_});
    return {};
}

std::expected<std::span<const std::uint8_t>, SegmentReadError>
SegmentReader::next() {
    while (current_index_ < segment_paths_.size()) {
        if (!page_reader_) {
            if (auto r = load_current_segment(); !r) {
                return std::unexpected(r.error());
            }
        }

        auto rec = page_reader_->next();
        if (rec) return *rec;

        const auto err = rec.error();
        const bool is_last_segment = (current_index_ + 1 == segment_paths_.size());

        if (err == WalReadError::EndOfStream) {
            // Clean exhaustion of this segment. Advance.
            page_reader_.reset();
            ++current_index_;
            continue;
        }
        if (err == WalReadError::TornRecord && is_last_segment) {
            // Crashed writer left a partial record at the tail of the last
            // segment. Treat as clean EOF and drop the torn data — matches
            // Go's tolerant replay behaviour for the active segment.
            page_reader_.reset();
            ++current_index_;
            continue;
        }
        // Any other error — including a torn record in a non-final segment
        // (which means the WAL is genuinely corrupted because records don't
        // span segments) — is fatal.
        if (err == WalReadError::TornRecord) {
            return std::unexpected(SegmentReadError::CorruptMidStream);
        }
        return std::unexpected(map_page_error(err));
    }
    return std::unexpected(SegmentReadError::EndOfStream);
}

}  // namespace merlion_tsdb::wal
