#include "merlion_tsdb/block/index_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>

#include "merlion_tsdb/encoding/crc32c.hpp"
#include "merlion_tsdb/encoding/varint.hpp"

namespace merlion_tsdb::block {

namespace {

std::error_code errno_ec() noexcept {
    return {errno, std::generic_category()};
}

}  // namespace

// --- Buffer helpers ---------------------------------------------------------

void IndexWriter::put_u8(std::uint8_t v) {
    buf_.push_back(v);
}

void IndexWriter::put_be32(std::uint32_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v >> 24));
    buf_.push_back(static_cast<std::uint8_t>(v >> 16));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
    buf_.push_back(static_cast<std::uint8_t>(v));
}

void IndexWriter::put_be64(std::uint64_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v >> 56));
    buf_.push_back(static_cast<std::uint8_t>(v >> 48));
    buf_.push_back(static_cast<std::uint8_t>(v >> 40));
    buf_.push_back(static_cast<std::uint8_t>(v >> 32));
    buf_.push_back(static_cast<std::uint8_t>(v >> 24));
    buf_.push_back(static_cast<std::uint8_t>(v >> 16));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
    buf_.push_back(static_cast<std::uint8_t>(v));
}

void IndexWriter::put_uvarint(std::uint64_t v) {
    std::array<std::uint8_t, varint::max_varint_len64> tmp{};
    const auto n = varint::put_uvarint(tmp, v);
    buf_.insert(buf_.end(), tmp.begin(), tmp.begin() + n);
}

void IndexWriter::put_varint(std::int64_t v) {
    std::array<std::uint8_t, varint::max_varint_len64> tmp{};
    const auto n = varint::put_varint(tmp, v);
    buf_.insert(buf_.end(), tmp.begin(), tmp.begin() + n);
}

void IndexWriter::put_uvarint_str(std::string_view s) {
    put_uvarint(s.size());
    buf_.insert(buf_.end(),
                reinterpret_cast<const std::uint8_t*>(s.data()),
                reinterpret_cast<const std::uint8_t*>(s.data() + s.size()));
}

void IndexWriter::patch_be32(std::size_t offset, std::uint32_t v) {
    assert(offset + 4 <= buf_.size());
    buf_[offset    ] = static_cast<std::uint8_t>(v >> 24);
    buf_[offset + 1] = static_cast<std::uint8_t>(v >> 16);
    buf_[offset + 2] = static_cast<std::uint8_t>(v >> 8);
    buf_[offset + 3] = static_cast<std::uint8_t>(v);
}

void IndexWriter::append_crc(std::size_t start, std::size_t end) {
    const auto crc = crc32c::compute(
        std::span<const std::uint8_t>{buf_.data() + start, end - start});
    put_be32(crc);
}

void IndexWriter::pad_to(std::size_t align) {
    const auto rem = buf_.size() % align;
    if (rem != 0) buf_.insert(buf_.end(), align - rem, std::uint8_t{0});
}

// --- Public API -------------------------------------------------------------

std::expected<IndexWriter, std::error_code>
IndexWriter::create(const std::filesystem::path& file) {
    if (std::filesystem::exists(file)) {
        return std::unexpected(std::make_error_code(std::errc::file_exists));
    }
    IndexWriter w{file};
    // Header: magic + version. The reader trusts these on open.
    w.put_be32(k_magic_index);
    w.put_u8(k_index_write_format);
    w.stage_ = Stage::Symbols;
    return w;
}

std::expected<std::uint32_t, std::error_code>
IndexWriter::add_symbol(std::string_view sym) {
    if (stage_ != Stage::Symbols) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    if (any_symbol_ && sym < std::string_view{last_symbol_}) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    // The first symbol triggers reservation of the section header
    // (length prefix + count) so we can patch them in finish_symbols().
    if (!any_symbol_) {
        off_symbols_ = buf_.size();
        put_be32(0);                  // length placeholder
        symbols_len_off_ = buf_.size();
        put_be32(0);                  // count placeholder (patched on finish)
    }
    put_uvarint_str(sym);
    any_symbol_ = true;
    last_symbol_.assign(sym);
    return next_sym_ref_++;
}

std::expected<void, std::error_code> IndexWriter::finish_symbols() {
    if (stage_ != Stage::Symbols) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    if (!any_symbol_) {
        // Reserve an empty symbol section so the TOC still has a valid offset.
        off_symbols_ = buf_.size();
        put_be32(0);                  // length placeholder
        symbols_len_off_ = buf_.size();
        put_be32(0);                  // count placeholder
    }
    // Patch count.
    patch_be32(symbols_len_off_, next_sym_ref_);
    // Compute payload length (everything since the length prefix, minus
    // the 4 bytes of the prefix itself).
    const auto payload_start = off_symbols_ + 4;
    const auto payload_end   = buf_.size();
    patch_be32(off_symbols_,
               static_cast<std::uint32_t>(payload_end - payload_start));
    // CRC32C over the payload only (the length prefix is NOT included).
    append_crc(payload_start, payload_end);

    stage_ = Stage::Series;
    return {};
}

std::expected<std::uint32_t, std::error_code>
IndexWriter::add_series(
    std::span<const std::pair<std::uint32_t, std::uint32_t>> label_refs,
    std::span<const ChunkMeta> chunk_metas) {
    if (stage_ != Stage::Series) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    // The first series triggers recording of the series-section offset.
    if (series_count_ == 0) {
        // Pad up to the 16-byte alignment so the very first series ID
        // (offset / 16) is well-defined.
        pad_to(k_index_series_align);
        off_series_ = buf_.size();
    } else {
        // Subsequent series also start on a 16-byte boundary so every
        // series ID is unique and the reader can derive offset = id * 16.
        pad_to(k_index_series_align);
    }

    const auto series_start = buf_.size();

    // Serialise the payload into a side buffer so we can prefix it with
    // its uvarint length.
    std::vector<std::uint8_t> payload;
    {
        // labels.
        std::array<std::uint8_t, varint::max_varint_len64> tmp{};
        auto append_u = [&](std::uint64_t v) {
            const auto n = varint::put_uvarint(tmp, v);
            payload.insert(payload.end(), tmp.begin(), tmp.begin() + n);
        };
        auto append_s = [&](std::int64_t v) {
            const auto n = varint::put_varint(tmp, v);
            payload.insert(payload.end(), tmp.begin(), tmp.begin() + n);
        };

        append_u(label_refs.size());
        for (const auto& [name_ref, val_ref] : label_refs) {
            append_u(name_ref);
            append_u(val_ref);
        }
        // chunks.
        append_u(chunk_metas.size());
        if (!chunk_metas.empty()) {
            const auto& first = chunk_metas[0];
            append_s(first.min_time);
            append_u(static_cast<std::uint64_t>(first.max_time - first.min_time));
            append_u(first.ref);
            std::int64_t t0 = first.max_time;
            std::int64_t ref0 = static_cast<std::int64_t>(first.ref);
            for (std::size_t i = 1; i < chunk_metas.size(); ++i) {
                const auto& c = chunk_metas[i];
                append_u(static_cast<std::uint64_t>(c.min_time - t0));
                append_u(static_cast<std::uint64_t>(c.max_time - c.min_time));
                append_s(static_cast<std::int64_t>(c.ref) - ref0);
                t0 = c.max_time;
                ref0 = static_cast<std::int64_t>(c.ref);
            }
        }
    }

    // Emit: uvarint length | payload | u32 BE CRC32C.
    put_uvarint(payload.size());
    const auto payload_off = buf_.size();
    buf_.insert(buf_.end(), payload.begin(), payload.end());
    append_crc(payload_off, payload_off + payload.size());

    const std::uint32_t series_id =
        static_cast<std::uint32_t>(series_start / k_index_series_align);
    ++series_count_;
    return series_id;
}

std::expected<void, std::error_code> IndexWriter::finish_series() {
    if (stage_ != Stage::Series) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    if (series_count_ == 0) {
        // Empty series section. Still record where it starts so the TOC
        // points somewhere sensible.
        off_series_ = buf_.size();
    }
    // Label-indices section is empty in V2/V3. Record offset for the TOC,
    // emit no bytes.
    pad_to(k_index_series_align);
    off_label_idx_ = buf_.size();
    off_label_tbl_ = buf_.size();
    off_postings_  = buf_.size();
    stage_ = Stage::Postings;
    return {};
}

std::expected<void, std::error_code>
IndexWriter::add_postings(std::string_view name,
                          std::string_view value,
                          std::span<const std::uint32_t> series_ids) {
    if (stage_ != Stage::Postings) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
    // Strictly-ascending check.
    for (std::size_t i = 1; i < series_ids.size(); ++i) {
        if (series_ids[i] <= series_ids[i - 1]) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
    }
    // 4-byte alignment for each list (upstream's writePosting line 848).
    pad_to(4);
    const auto list_off = buf_.size();
    // u32 BE length placeholder.
    put_be32(0);
    const auto payload_off = buf_.size();
    // u32 BE count then u32 BE × series_ids.
    put_be32(static_cast<std::uint32_t>(series_ids.size()));
    for (auto id : series_ids) put_be32(id);
    const auto payload_end = buf_.size();
    patch_be32(list_off,
               static_cast<std::uint32_t>(payload_end - payload_off));
    append_crc(payload_off, payload_end);

    pending_postings_.push_back({std::string(name), std::string(value), list_off});
    return {};
}

std::expected<void, std::error_code> IndexWriter::close() {
    if (stage_ != Stage::Postings) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    // Postings offset table.
    off_postings_tbl_ = buf_.size();
    put_be32(0);                       // length placeholder
    const auto tbl_payload_off = buf_.size();
    put_be32(static_cast<std::uint32_t>(pending_postings_.size()));
    for (const auto& e : pending_postings_) {
        put_uvarint(2);                // keycount, always 2 upstream
        put_uvarint_str(e.name);
        put_uvarint_str(e.value);
        put_uvarint(e.offset);
    }
    const auto tbl_payload_end = buf_.size();
    patch_be32(off_postings_tbl_,
               static_cast<std::uint32_t>(tbl_payload_end - tbl_payload_off));
    append_crc(tbl_payload_off, tbl_payload_end);

    // TOC: 6 u64 BE offsets + u32 BE CRC32C over the 48-byte body.
    const auto toc_start = buf_.size();
    put_be64(off_symbols_);
    put_be64(off_series_);
    put_be64(off_label_idx_);
    put_be64(off_label_tbl_);
    put_be64(off_postings_);
    put_be64(off_postings_tbl_);
    append_crc(toc_start, toc_start + 48);

    // Flush buffer to disk.
    const int fd = ::open(file_.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) return std::unexpected(errno_ec());
    std::span<const std::uint8_t> remaining{buf_};
    while (!remaining.empty()) {
        const auto n = ::write(fd, remaining.data(), remaining.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            const auto ec = errno_ec();
            ::close(fd);
            return std::unexpected(ec);
        }
        remaining = remaining.subspan(static_cast<std::size_t>(n));
    }
    if (::fsync(fd) < 0) {
        const auto ec = errno_ec();
        ::close(fd);
        return std::unexpected(ec);
    }
    if (::close(fd) < 0) return std::unexpected(errno_ec());

    stage_ = Stage::Done;
    return {};
}

}  // namespace merlion_tsdb::block
