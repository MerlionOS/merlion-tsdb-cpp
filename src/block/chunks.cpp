#include "merlion_tsdb/block/chunks.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

#include "merlion_tsdb/encoding/crc32c.hpp"
#include "merlion_tsdb/encoding/varint.hpp"

namespace merlion_tsdb::block {

namespace {

std::error_code errno_ec() noexcept {
    return {errno, std::generic_category()};
}

std::string format_segment_name(std::uint32_t seq) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06u", seq);
    return buf;
}

std::expected<std::uint32_t, std::errc>
parse_segment_name(std::string_view name) noexcept {
    if (name.size() != k_chunks_segment_filename_width) {
        return std::unexpected(std::errc::invalid_argument);
    }
    std::uint32_t v = 0;
    auto r = std::from_chars(name.data(), name.data() + name.size(), v);
    if (r.ec != std::errc{} || r.ptr != name.data() + name.size()) {
        return std::unexpected(std::errc::invalid_argument);
    }
    return v;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

std::uint32_t read_be32(std::span<const std::uint8_t> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24) |
           (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8)  |
            static_cast<std::uint32_t>(b[3]);
}

}  // namespace

// --- ChunkWriter ------------------------------------------------------------

ChunkWriter::ChunkWriter(std::filesystem::path dir, std::size_t segment_size) noexcept
    : dir_(std::move(dir)), segment_size_(segment_size) {}

ChunkWriter::ChunkWriter(ChunkWriter&& other) noexcept
    : dir_(std::move(other.dir_)),
      segment_size_(other.segment_size_),
      fd_(other.fd_),
      seq_(other.seq_),
      seg_offset_(other.seg_offset_),
      closed_(other.closed_) {
    other.fd_     = -1;
    other.closed_ = true;
}

ChunkWriter& ChunkWriter::operator=(ChunkWriter&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            (void)::fsync(fd_);
            ::close(fd_);
        }
        dir_          = std::move(other.dir_);
        segment_size_ = other.segment_size_;
        fd_           = other.fd_;
        seq_          = other.seq_;
        seg_offset_   = other.seg_offset_;
        closed_       = other.closed_;
        other.fd_     = -1;
        other.closed_ = true;
    }
    return *this;
}

ChunkWriter::~ChunkWriter() {
    if (!closed_ && fd_ >= 0) {
        (void)::fsync(fd_);
        ::close(fd_);
    }
}

std::expected<ChunkWriter, std::error_code>
ChunkWriter::open(const std::filesystem::path& dir, std::size_t segment_size) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return std::unexpected(ec);

    // Each block writes a fresh chunks/ directory; resume-after-crash is
    // unnecessary for block writes (the whole block is rewritten on
    // compaction failure). Match upstream's writer: refuse non-empty dirs.
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return std::unexpected(ec);
        if (!entry.is_regular_file()) continue;
        if (parse_segment_name(entry.path().filename().string())) {
            return std::unexpected(std::make_error_code(std::errc::file_exists));
        }
    }
    if (ec) return std::unexpected(ec);

    ChunkWriter w(dir, segment_size);
    w.seq_ = 0;  // 0-indexed array position; matches upstream's BlockChunkRef.
    if (auto r = w.cut_new_segment(); !r) return std::unexpected(r.error());
    return w;
}

std::expected<void, std::error_code> ChunkWriter::cut_new_segment() {
    if (fd_ >= 0) {
        if (::fsync(fd_) < 0) return std::unexpected(errno_ec());
        if (::close(fd_) < 0) return std::unexpected(errno_ec());
        fd_ = -1;
        ++seq_;
    }
    // Filename is (seq + 1) zero-padded to 6 digits — upstream convention.
    // The ChunkRef's `seq` field is the 0-indexed value `seq_`.
    const auto path = dir_ / format_segment_name(seq_ + 1U);
    fd_ = ::open(path.c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd_ < 0) return std::unexpected(errno_ec());

    // Emit the segment header.
    std::array<std::uint8_t, k_chunks_segment_header_size> header{};
    header[0] = static_cast<std::uint8_t>(k_magic_chunks >> 24);
    header[1] = static_cast<std::uint8_t>(k_magic_chunks >> 16);
    header[2] = static_cast<std::uint8_t>(k_magic_chunks >> 8);
    header[3] = static_cast<std::uint8_t>(k_magic_chunks);
    header[4] = k_chunks_format_v1;
    // header[5..7] already zero
    if (auto r = write_all(header); !r) return r;
    seg_offset_ = k_chunks_segment_header_size;
    return {};
}

std::expected<void, std::error_code>
ChunkWriter::write_all(std::span<const std::uint8_t> data) {
    while (!data.empty()) {
        auto n = ::write(fd_, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(errno_ec());
        }
        if (n == 0) return std::unexpected(std::make_error_code(std::errc::io_error));
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

std::expected<BlockChunkRef, std::error_code>
ChunkWriter::write(chunkenc::Encoding enc, std::span<const std::uint8_t> data) {
    if (closed_) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    // Worst-case framed size for the rollover check.
    const std::size_t worst_case =
        varint::max_varint_len64 + k_chunk_encoding_size + data.size() + k_chunk_crc_size;
    if (seg_offset_ > k_chunks_segment_header_size &&
        seg_offset_ + worst_case > segment_size_) {
        if (auto r = cut_new_segment(); !r) return std::unexpected(r.error());
    }

    const BlockChunkRef ref{seq_, static_cast<std::uint32_t>(seg_offset_)};

    // Serialise everything to a single buffer then write once — keeps the
    // CRC computation and disk write decoupled and simplifies error
    // recovery if the write fails mid-way.
    std::vector<std::uint8_t> buf;
    buf.reserve(worst_case);

    std::array<std::uint8_t, varint::max_varint_len64> vbuf{};
    const auto vn = varint::put_uvarint(vbuf, data.size());
    buf.insert(buf.end(), vbuf.begin(), vbuf.begin() + vn);

    buf.push_back(static_cast<std::uint8_t>(enc));
    buf.insert(buf.end(), data.begin(), data.end());

    // CRC32C over encoding + data (length is NOT included; matches
    // Meta::writeHash in upstream).
    auto crc_state = crc32c::k_init;
    crc_state = crc32c::update(crc_state, std::span<const std::uint8_t>{&buf[vn], 1});
    crc_state = crc32c::update(crc_state, data);
    append_be32(buf, crc32c::finalize(crc_state));

    if (auto r = write_all(buf); !r) return std::unexpected(r.error());
    seg_offset_ += buf.size();
    return ref;
}

std::expected<void, std::error_code> ChunkWriter::close() {
    if (closed_) return {};
    closed_ = true;
    if (fd_ < 0) return {};
    if (::fsync(fd_) < 0) return std::unexpected(errno_ec());
    if (::close(fd_) < 0) return std::unexpected(errno_ec());
    fd_ = -1;
    return {};
}

// --- ChunkReader ------------------------------------------------------------

std::expected<ChunkReader, std::error_code>
ChunkReader::open(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (ec) return std::unexpected(ec);
        return std::unexpected(std::make_error_code(std::errc::not_a_directory));
    }

    // Collect path-and-filename-id pairs, sort by filename id ascending, then
    // RE-INDEX so the public-facing `seq` is the 0-indexed array position.
    // This matches upstream's BlockChunkRef.Unpack (chunks/chunks.go:113) —
    // the unpacked seq is `bs[seq]` in the segment slice array, not the
    // numeric filename.
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> raw;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return std::unexpected(ec);
        if (!entry.is_regular_file()) continue;
        auto parsed = parse_segment_name(entry.path().filename().string());
        if (!parsed) continue;
        raw.emplace_back(*parsed, entry.path());
    }
    std::sort(raw.begin(), raw.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<std::uint32_t, std::filesystem::path>> segs;
    segs.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        segs.emplace_back(static_cast<std::uint32_t>(i), std::move(raw[i].second));
    }

    ChunkReader r(std::move(segs));
    r.segment_bytes_cache_.resize(r.segments_.size());
    return r;
}

std::vector<std::uint32_t> ChunkReader::segment_seqs() const {
    std::vector<std::uint32_t> out;
    out.reserve(segments_.size());
    for (const auto& [s, _] : segments_) out.push_back(s);
    return out;
}

std::expected<std::span<const std::uint8_t>, std::error_code>
ChunkReader::bytes_for(std::uint32_t seq) const {
    if (seq >= segment_bytes_cache_.size()) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    auto& cached = segment_bytes_cache_[seq];
    if (!cached.empty()) {
        return std::span<const std::uint8_t>{cached};
    }
    // Direct 0-indexed lookup — segments_ is dense from 0..size()-1 after
    // open() re-indexed.
    std::ifstream in(segments_[seq].second, std::ios::binary);
    if (!in) return std::unexpected(errno_ec());
    cached.assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>{});
    if (in.bad()) return std::unexpected(std::make_error_code(std::errc::io_error));

    // Validate the segment header before letting any reader trust the bytes.
    if (cached.size() < k_chunks_segment_header_size) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    if (read_be32(std::span<const std::uint8_t>{cached}.subspan(0, 4)) != k_magic_chunks) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    if (cached[4] != k_chunks_format_v1) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    return std::span<const std::uint8_t>{cached};
}

std::expected<ChunkPayload, std::error_code>
ChunkReader::read(BlockChunkRef ref) const {
    auto bytes_or = bytes_for(ref.seq);
    if (!bytes_or) return std::unexpected(bytes_or.error());
    const auto bytes = *bytes_or;

    if (ref.offset >= bytes.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    // Read length uvarint.
    auto len_r = varint::read_uvarint(bytes.subspan(ref.offset));
    if (!len_r) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto length = static_cast<std::size_t>(len_r->first);
    const auto enc_off = ref.offset + len_r->second;
    const auto data_off = enc_off + k_chunk_encoding_size;
    const auto crc_off  = data_off + length;
    const auto end_off  = crc_off + k_chunk_crc_size;
    if (end_off > bytes.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }

    const auto enc      = static_cast<chunkenc::Encoding>(bytes[enc_off]);
    const auto data     = bytes.subspan(data_off, length);
    const auto stored   = read_be32(bytes.subspan(crc_off, 4));
    auto crc_state = crc32c::k_init;
    crc_state = crc32c::update(crc_state, std::span<const std::uint8_t>{&bytes[enc_off], 1});
    crc_state = crc32c::update(crc_state, data);
    if (crc32c::finalize(crc_state) != stored) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }

    ChunkPayload out;
    out.encoding = enc;
    out.data.assign(data.begin(), data.end());
    return out;
}

std::expected<std::vector<ChunkReader::ChunkInfo>, std::error_code>
ChunkReader::iterate_segment(std::uint32_t seq) const {
    auto bytes_or = bytes_for(seq);
    if (!bytes_or) return std::unexpected(bytes_or.error());
    const auto bytes = *bytes_or;

    std::vector<ChunkInfo> out;
    std::size_t offset = k_chunks_segment_header_size;
    while (offset < bytes.size()) {
        auto len_r = varint::read_uvarint(bytes.subspan(offset));
        if (!len_r) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        ChunkInfo info{};
        info.ref       = {seq, static_cast<std::uint32_t>(offset)};
        info.data_size = static_cast<std::size_t>(len_r->first);
        const auto enc_off = offset + len_r->second;
        if (enc_off + k_chunk_encoding_size + info.data_size + k_chunk_crc_size >
            bytes.size()) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        info.encoding  = static_cast<chunkenc::Encoding>(bytes[enc_off]);
        out.push_back(info);
        offset = enc_off + k_chunk_encoding_size + info.data_size + k_chunk_crc_size;
    }
    return out;
}

}  // namespace merlion_tsdb::block
