#include "merlion_tsdb/wal/segment_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <utility>

namespace merlion_tsdb::wal {

namespace {

// Segment file name is exactly 8 zero-padded decimal digits. Anything else
// in the directory (lockfile, partial renames, etc.) is ignored.
constexpr std::size_t k_segment_name_width = 8;

std::string format_segment_name(std::size_t idx) {
    char buf[16];  // 8 digits + null + slack
    std::snprintf(buf, sizeof(buf), "%08zu", idx);
    return buf;
}

std::expected<std::size_t, std::error_code>
parse_segment_name(std::string_view name) noexcept {
    if (name.size() != k_segment_name_width) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    std::size_t v = 0;
    auto r = std::from_chars(name.data(), name.data() + name.size(), v);
    if (r.ec != std::errc{} || r.ptr != name.data() + name.size()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    return v;
}

std::error_code errno_ec() noexcept {
    return std::error_code(errno, std::generic_category());
}

std::expected<void, std::error_code>
write_all(int fd, std::span<const std::uint8_t> data) noexcept {
    while (!data.empty()) {
        const auto n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(errno_ec());
        }
        if (n == 0) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

}  // namespace

SegmentWriter::SegmentWriter(std::filesystem::path dir, std::size_t segment_size,
                             std::size_t initial_index) noexcept
    : dir_(std::move(dir)), segment_size_(segment_size), seg_index_(initial_index) {}

SegmentWriter::SegmentWriter(SegmentWriter&& other) noexcept
    : dir_(std::move(other.dir_)),
      segment_size_(other.segment_size_),
      seg_index_(other.seg_index_),
      fd_(other.fd_),
      pw_(std::move(other.pw_)),
      flushed_bytes_(other.flushed_bytes_) {
    other.fd_ = -1;
    other.flushed_bytes_ = 0;
}

SegmentWriter& SegmentWriter::operator=(SegmentWriter&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        dir_ = std::move(other.dir_);
        segment_size_ = other.segment_size_;
        seg_index_ = other.seg_index_;
        fd_ = other.fd_;
        pw_ = std::move(other.pw_);
        flushed_bytes_ = other.flushed_bytes_;
        other.fd_ = -1;
        other.flushed_bytes_ = 0;
    }
    return *this;
}

SegmentWriter::~SegmentWriter() {
    if (fd_ >= 0) {
        // Best-effort: flush whatever is buffered and fsync. Errors are
        // swallowed because destructors must not throw and the caller
        // didn't ask. Tests should explicitly call cut()/sync() to assert.
        auto _ = flush_pending();
        ::fsync(fd_);
        ::close(fd_);
    }
}

std::expected<SegmentWriter, std::error_code>
SegmentWriter::open(const std::filesystem::path& dir, std::size_t segment_size) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return std::unexpected(ec);

    std::size_t next_index = 0;
    bool any_found = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return std::unexpected(ec);
        if (!entry.is_regular_file()) continue;
        auto parsed = parse_segment_name(entry.path().filename().string());
        if (!parsed) continue;
        any_found = true;
        next_index = std::max(next_index, *parsed + 1);
    }
    if (ec) return std::unexpected(ec);
    if (!any_found) next_index = 0;

    SegmentWriter writer(dir, segment_size, next_index);
    if (auto r = writer.open_new_segment(); !r) {
        return std::unexpected(r.error());
    }
    return writer;
}

std::expected<void, std::error_code> SegmentWriter::open_new_segment() {
    const auto path = dir_ / format_segment_name(seg_index_);
    const int fd = ::open(path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) return std::unexpected(errno_ec());
    fd_ = fd;
    pw_ = PageWriter{};
    flushed_bytes_ = 0;
    return {};
}

std::filesystem::path SegmentWriter::current_segment_path() const {
    return dir_ / format_segment_name(seg_index_);
}

std::size_t
SegmentWriter::framed_size_upper_bound(std::size_t body_len) const noexcept {
    // Each fragment has a 7-byte header. Up to (k_page_size - 7) body bytes
    // per page. Add up to (k_record_header_size - 1) bytes of conservative
    // padding accounted for the case where the current page is nearly full.
    const auto body_per_page = k_page_size - k_record_header_size;
    const auto num_fragments =
        body_len == 0 ? std::size_t{1}
                      : (body_len + body_per_page - 1) / body_per_page;
    return num_fragments * k_record_header_size + body_len
         + (k_record_header_size - 1);
}

std::expected<void, std::error_code>
SegmentWriter::log(std::span<const std::uint8_t> body) {
    // Rollover decision: if the worst-case framed size would push the
    // current segment past its size limit, cut now. Only roll if at least
    // one record has already been written to the current segment — otherwise
    // a record larger than segment_size_ would loop forever.
    const auto already = flushed_bytes_ + (pw_.bytes().size() - flushed_bytes_);
    const auto upper_bound = framed_size_upper_bound(body.size());
    if (already > 0 && already + upper_bound > segment_size_) {
        if (auto r = cut(); !r) return r;
    }

    pw_.log(body);
    return flush_pending();
}

std::expected<void, std::error_code> SegmentWriter::flush_pending() {
    if (fd_ < 0) return {};
    const auto buf = pw_.bytes();
    if (flushed_bytes_ >= buf.size()) return {};
    const auto pending = buf.subspan(flushed_bytes_);
    if (auto r = write_all(fd_, pending); !r) return r;
    flushed_bytes_ = buf.size();
    return {};
}

std::expected<void, std::error_code> SegmentWriter::sync() {
    if (fd_ < 0) return {};
    if (auto r = flush_pending(); !r) return r;
    if (::fsync(fd_) < 0) return std::unexpected(errno_ec());
    return {};
}

std::expected<void, std::error_code> SegmentWriter::cut() {
    if (fd_ < 0) return {};

    // Pad the trailing partial page so the on-disk segment ends at a page
    // boundary. The reader will see the PageTerm zero byte and stop.
    pw_.close_page();
    if (auto r = flush_pending(); !r) return r;

    if (::fsync(fd_) < 0) return std::unexpected(errno_ec());
    if (::close(fd_) < 0) return std::unexpected(errno_ec());
    fd_ = -1;

    ++seg_index_;
    return open_new_segment();
}

int SegmentWriter::release_fd() noexcept {
    const int fd = fd_;
    fd_ = -1;
    flushed_bytes_ = 0;
    pw_ = PageWriter{};
    return fd;
}

}  // namespace merlion_tsdb::wal
