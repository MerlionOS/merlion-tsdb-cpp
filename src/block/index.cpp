#include "merlion_tsdb/block/index.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include "merlion_tsdb/encoding/crc32c.hpp"
#include "merlion_tsdb/encoding/varint.hpp"

namespace merlion_tsdb::block {

namespace {

constexpr std::size_t k_symbol_sparse_stride = 32;  // matches upstream symbolFactor

std::uint32_t read_be32(std::span<const std::uint8_t> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24) |
           (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8)  |
            static_cast<std::uint32_t>(b[3]);
}

std::uint64_t read_be64(std::span<const std::uint8_t> b) noexcept {
    return (static_cast<std::uint64_t>(b[0]) << 56) |
           (static_cast<std::uint64_t>(b[1]) << 48) |
           (static_cast<std::uint64_t>(b[2]) << 40) |
           (static_cast<std::uint64_t>(b[3]) << 32) |
           (static_cast<std::uint64_t>(b[4]) << 24) |
           (static_cast<std::uint64_t>(b[5]) << 16) |
           (static_cast<std::uint64_t>(b[6]) << 8)  |
            static_cast<std::uint64_t>(b[7]);
}

}  // namespace

namespace detail {

std::expected<std::span<const std::uint8_t>, std::error_code>
read_section_payload(std::span<const std::uint8_t> file_bytes,
                     std::size_t offset) {
    if (offset + 4 > file_bytes.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto len = read_be32(file_bytes.subspan(offset, 4));
    const auto payload_off = offset + 4;
    if (payload_off + len + 4 > file_bytes.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto payload = file_bytes.subspan(payload_off, len);
    const auto stored_crc =
        read_be32(file_bytes.subspan(payload_off + len, 4));
    if (crc32c::compute(payload) != stored_crc) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    return payload;
}

std::expected<std::span<const std::uint8_t>, std::error_code>
read_uvarint_section_payload(std::span<const std::uint8_t> file_bytes,
                             std::size_t offset) {
    if (offset >= file_bytes.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    auto len_r = varint::read_uvarint(file_bytes.subspan(offset));
    if (!len_r) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto payload_off = offset + len_r->second;
    const auto len = static_cast<std::size_t>(len_r->first);
    if (payload_off + len + 4 > file_bytes.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto payload = file_bytes.subspan(payload_off, len);
    const auto stored_crc =
        read_be32(file_bytes.subspan(payload_off + len, 4));
    if (crc32c::compute(payload) != stored_crc) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    return payload;
}

std::expected<SeriesEntry, std::error_code>
decode_series_payload(std::span<const std::uint8_t> payload,
                      const IndexSymbolTable& symbols) {
    std::size_t pos = 0;
    auto read_uv = [&](std::uint64_t& out) -> bool {
        auto r = varint::read_uvarint(payload.subspan(pos));
        if (!r) return false;
        out = r->first;
        pos += r->second;
        return true;
    };
    auto read_v = [&](std::int64_t& out) -> bool {
        auto r = varint::read_varint(payload.subspan(pos));
        if (!r) return false;
        out = r->first;
        pos += r->second;
        return true;
    };

    SeriesEntry entry;
    std::uint64_t k = 0;
    if (!read_uv(k)) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    std::vector<model::Label> labels;
    labels.reserve(static_cast<std::size_t>(k));
    for (std::uint64_t i = 0; i < k; ++i) {
        std::uint64_t lno = 0;
        std::uint64_t lvo = 0;
        if (!read_uv(lno) || !read_uv(lvo)) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        auto name = symbols.lookup(static_cast<std::uint32_t>(lno));
        auto value = symbols.lookup(static_cast<std::uint32_t>(lvo));
        if (!name || !value) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        labels.push_back({std::string{*name}, std::string{*value}});
    }
    entry.labels = model::Labels{std::move(labels)};

    // Chunks.
    if (!read_uv(k)) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    if (k == 0) return entry;

    entry.chunks.reserve(static_cast<std::size_t>(k));
    std::int64_t t0 = 0;
    if (!read_v(t0)) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    std::uint64_t maxt_delta = 0;
    if (!read_uv(maxt_delta)) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    std::int64_t maxt = static_cast<std::int64_t>(maxt_delta) + t0;
    std::uint64_t ref0 = 0;
    if (!read_uv(ref0)) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    entry.chunks.push_back({t0, maxt, ref0});
    t0 = maxt;
    std::int64_t signed_ref = static_cast<std::int64_t>(ref0);

    for (std::uint64_t i = 1; i < k; ++i) {
        std::uint64_t mint_delta = 0;
        if (!read_uv(mint_delta)) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        const std::int64_t mint = static_cast<std::int64_t>(mint_delta) + t0;
        std::uint64_t maxt_d = 0;
        if (!read_uv(maxt_d)) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        const std::int64_t this_maxt = static_cast<std::int64_t>(maxt_d) + mint;
        std::int64_t ref_delta = 0;
        if (!read_v(ref_delta)) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        signed_ref += ref_delta;
        entry.chunks.push_back({mint, this_maxt,
                                static_cast<std::uint64_t>(signed_ref)});
        t0 = this_maxt;
    }
    return entry;
}

std::expected<IndexTOC, std::error_code>
parse_toc(std::span<const std::uint8_t> file_bytes) {
    if (file_bytes.size() < k_index_toc_size) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto toc_start = file_bytes.size() - k_index_toc_size;
    const auto toc_bytes = file_bytes.subspan(toc_start, k_index_toc_size);
    // CRC is over the 6 u64 fields (the first 48 bytes of the TOC).
    const auto stored_crc = read_be32(toc_bytes.subspan(48, 4));
    if (crc32c::compute(toc_bytes.subspan(0, 48)) != stored_crc) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    IndexTOC toc;
    toc.symbols             = read_be64(toc_bytes.subspan( 0, 8));
    toc.series              = read_be64(toc_bytes.subspan( 8, 8));
    toc.label_indices       = read_be64(toc_bytes.subspan(16, 8));
    toc.label_indices_table = read_be64(toc_bytes.subspan(24, 8));
    toc.postings            = read_be64(toc_bytes.subspan(32, 8));
    toc.postings_table      = read_be64(toc_bytes.subspan(40, 8));
    return toc;
}

}  // namespace detail

// --- IndexSymbolTable -------------------------------------------------------

IndexSymbolTable::IndexSymbolTable(std::span<const std::uint8_t> file_bytes,
                                   std::uint8_t version,
                                   std::uint32_t count,
                                   std::size_t payload_offset,
                                   std::vector<std::size_t> sparse_offsets) noexcept
    : file_bytes_(file_bytes),
      version_(version),
      count_(count),
      payload_offset_(payload_offset),
      sparse_offsets_(std::move(sparse_offsets)) {}

std::expected<IndexSymbolTable, std::error_code>
IndexSymbolTable::parse(std::span<const std::uint8_t> file_bytes,
                        std::size_t section_offset,
                        std::uint8_t version) {
    auto payload_or = detail::read_section_payload(file_bytes, section_offset);
    if (!payload_or) return std::unexpected(payload_or.error());
    const auto payload = *payload_or;
    if (payload.size() < 4) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const std::uint32_t count = read_be32(payload.subspan(0, 4));
    // Pre-compute the V3 sparse offset table. For V1 it costs nothing
    // (we still build it but only use it for iteration helpers).
    std::vector<std::size_t> offsets;
    offsets.reserve(1 + count / k_symbol_sparse_stride);
    const auto payload_off = section_offset + 4;       // absolute offset of count field
    const auto data_start  = payload_off + 4;          // absolute offset of first symbol
    std::size_t pos = data_start;
    std::uint32_t seen = 0;
    while (seen < count) {
        if (seen % k_symbol_sparse_stride == 0) offsets.push_back(pos);
        if (pos >= file_bytes.size()) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        // Read uvarint length and advance past the bytes.
        auto v = varint::read_uvarint(file_bytes.subspan(pos));
        if (!v) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        pos += v->second + static_cast<std::size_t>(v->first);
        if (pos > data_start + payload.size() - 4) {  // payload bytes minus the leading count
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        ++seen;
    }
    return IndexSymbolTable(file_bytes, version, count, payload_off, std::move(offsets));
}

std::expected<std::string_view, std::error_code>
IndexSymbolTable::lookup(std::uint32_t ref) const {
    std::size_t pos = 0;
    if (version_ == k_index_format_v1) {
        // V1: ref is an absolute file offset pointing at the uvarint length
        // of the symbol entry.
        pos = ref;
        if (pos >= file_bytes_.size()) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
    } else {
        // V2 / V3: ref is a 0-based index. Use the sparse offset table to
        // jump to the nearest reference point, then walk forward.
        if (ref >= count_) {
            return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
        }
        const auto bucket = ref / k_symbol_sparse_stride;
        const auto within = ref % k_symbol_sparse_stride;
        if (bucket >= sparse_offsets_.size()) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        pos = sparse_offsets_[bucket];
        for (std::uint32_t i = 0; i < within; ++i) {
            auto v = varint::read_uvarint(file_bytes_.subspan(pos));
            if (!v) {
                return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
            }
            pos += v->second + static_cast<std::size_t>(v->first);
        }
    }
    auto v = varint::read_uvarint(file_bytes_.subspan(pos));
    if (!v) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const auto str_start = pos + v->second;
    const auto str_len   = static_cast<std::size_t>(v->first);
    if (str_start + str_len > file_bytes_.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    return std::string_view(
        reinterpret_cast<const char*>(file_bytes_.data() + str_start), str_len);
}

std::vector<std::string> IndexSymbolTable::all_symbols() const {
    std::vector<std::string> out;
    out.reserve(count_);
    // payload_offset_ points at the count u32; symbols start 4 bytes later.
    std::size_t pos = payload_offset_ + 4;
    for (std::uint32_t i = 0; i < count_; ++i) {
        auto v = varint::read_uvarint(file_bytes_.subspan(pos));
        if (!v) break;
        const auto len = static_cast<std::size_t>(v->first);
        out.emplace_back(reinterpret_cast<const char*>(file_bytes_.data() + pos + v->second), len);
        pos += v->second + len;
    }
    return out;
}

// --- IndexReader ------------------------------------------------------------

IndexReader::IndexReader(std::vector<std::uint8_t> data,
                         std::uint8_t version,
                         IndexTOC toc,
                         IndexSymbolTable symbols,
                         PostingsOffsetTable postings_table) noexcept
    : data_(std::move(data)),
      version_(version),
      toc_(toc),
      symbols_(std::move(symbols)),
      postings_table_(std::move(postings_table)) {}

std::expected<IndexReader, std::error_code>
IndexReader::open(const std::filesystem::path& index_file) {
    std::ifstream in(index_file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    std::vector<std::uint8_t> data(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>{});
    if (in.bad()) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    std::span<const std::uint8_t> bytes{data};

    if (bytes.size() < k_index_header_size + k_index_toc_size) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    if (read_be32(bytes.subspan(0, 4)) != k_magic_index) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const std::uint8_t version = bytes[4];
    if (version != k_index_format_v1 &&
        version != k_index_format_v2 &&
        version != k_index_format_v3) {
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }

    auto toc_or = detail::parse_toc(bytes);
    if (!toc_or) return std::unexpected(toc_or.error());
    const auto toc = *toc_or;

    // Parse against a span into `data` BEFORE moving — the std::vector move
    // guarantee keeps the underlying buffer stable, so the span stays valid
    // after we move `data` into the IndexReader.
    auto symbols_or = IndexSymbolTable::parse(
        std::span<const std::uint8_t>{data},
        static_cast<std::size_t>(toc.symbols),
        version);
    if (!symbols_or) return std::unexpected(symbols_or.error());

    auto pot_or = PostingsOffsetTable::parse(
        std::span<const std::uint8_t>{data},
        static_cast<std::size_t>(toc.postings_table));
    if (!pot_or) return std::unexpected(pot_or.error());

    return IndexReader{std::move(data), version, toc,
                       std::move(*symbols_or), std::move(*pot_or)};
}

std::expected<std::vector<std::uint32_t>, std::error_code>
IndexReader::postings(std::string_view name, std::string_view value) const {
    auto off = postings_table_.lookup(name, value);
    if (!off) return std::vector<std::uint32_t>{};
    return read_posting_list(std::span<const std::uint8_t>{data_}, *off);
}

std::expected<std::vector<std::uint32_t>, std::error_code>
IndexReader::postings_for_name(std::string_view name) const {
    std::vector<std::uint32_t> out;
    for (const auto& e : postings_table_.entries()) {
        if (e.name != name) continue;
        auto ids = read_posting_list(std::span<const std::uint8_t>{data_}, e.offset);
        if (!ids) return std::unexpected(ids.error());
        // Merge sorted runs — each posting list is itself sorted, so a
        // single sort+unique at the end is fine (lists are small in
        // typical blocks).
        out.insert(out.end(), ids->begin(), ids->end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::expected<std::vector<std::uint32_t>, std::error_code>
IndexReader::all_postings() const {
    std::vector<std::uint32_t> out;
    for (const auto& e : postings_table_.entries()) {
        auto ids = read_posting_list(std::span<const std::uint8_t>{data_}, e.offset);
        if (!ids) return std::unexpected(ids.error());
        out.insert(out.end(), ids->begin(), ids->end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string>
IndexReader::label_values(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& e : postings_table_.entries()) {
        if (e.name == name) out.push_back(e.value);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::expected<SeriesEntry, std::error_code>
IndexReader::series(std::uint64_t id) const {
    // V1: id is the absolute file offset. V2/V3: actual offset = id * 16
    // (series entries are 16-byte aligned).
    const std::size_t offset = (version_ == k_index_format_v1)
                                   ? static_cast<std::size_t>(id)
                                   : static_cast<std::size_t>(id) * 16U;
    auto payload_or = detail::read_uvarint_section_payload(
        std::span<const std::uint8_t>{data_}, offset);
    if (!payload_or) return std::unexpected(payload_or.error());
    return detail::decode_series_payload(*payload_or, symbols_);
}

// --- PostingsOffsetTable ----------------------------------------------------

std::expected<PostingsOffsetTable, std::error_code>
PostingsOffsetTable::parse(std::span<const std::uint8_t> file_bytes,
                           std::size_t section_offset) {
    auto payload_or = detail::read_section_payload(file_bytes, section_offset);
    if (!payload_or) return std::unexpected(payload_or.error());
    const auto payload = *payload_or;
    if (payload.size() < 4) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const std::uint32_t count = read_be32(payload.subspan(0, 4));
    std::vector<PostingsOffsetEntry> entries;
    entries.reserve(count);

    std::size_t pos = 4;  // skip the leading count
    for (std::uint32_t i = 0; i < count; ++i) {
        auto keycount_or = varint::read_uvarint(payload.subspan(pos));
        if (!keycount_or) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        pos += keycount_or->second;
        // Upstream always writes 2 (name + value); be strict about that
        // until a non-2 case actually exists in real data.
        if (keycount_or->first != 2) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        // Read label name (uvarint length + bytes).
        auto namelen_or = varint::read_uvarint(payload.subspan(pos));
        if (!namelen_or) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        pos += namelen_or->second;
        const auto namelen = static_cast<std::size_t>(namelen_or->first);
        if (pos + namelen > payload.size()) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        std::string name(reinterpret_cast<const char*>(payload.data() + pos), namelen);
        pos += namelen;

        // Label value.
        auto vallen_or = varint::read_uvarint(payload.subspan(pos));
        if (!vallen_or) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        pos += vallen_or->second;
        const auto vallen = static_cast<std::size_t>(vallen_or->first);
        if (pos + vallen > payload.size()) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        std::string value(reinterpret_cast<const char*>(payload.data() + pos), vallen);
        pos += vallen;

        // Offset.
        auto off_or = varint::read_uvarint(payload.subspan(pos));
        if (!off_or) {
            return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
        }
        pos += off_or->second;

        entries.push_back(
            {std::move(name), std::move(value), off_or->first});
    }
    if (pos != payload.size()) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    return PostingsOffsetTable{std::move(entries)};
}

PostingsOffsetTable::PostingsOffsetTable(std::vector<PostingsOffsetEntry> entries)
    : entries_(std::move(entries)) {
    by_pair_.reserve(entries_.size());
    for (const auto& e : entries_) {
        std::string key;
        key.reserve(e.name.size() + 1 + e.value.size());
        key.append(e.name);
        key.push_back('\0');
        key.append(e.value);
        by_pair_.emplace(std::move(key), e.offset);
    }
}

std::optional<std::uint64_t>
PostingsOffsetTable::lookup(std::string_view name,
                            std::string_view value) const {
    std::string key;
    key.reserve(name.size() + 1 + value.size());
    key.append(name);
    key.push_back('\0');
    key.append(value);
    auto it = by_pair_.find(key);
    if (it == by_pair_.end()) return std::nullopt;
    return it->second;
}

// --- Posting list -----------------------------------------------------------

std::expected<std::vector<std::uint32_t>, std::error_code>
read_posting_list(std::span<const std::uint8_t> file_bytes, std::uint64_t offset) {
    auto payload_or = detail::read_section_payload(file_bytes, static_cast<std::size_t>(offset));
    if (!payload_or) return std::unexpected(payload_or.error());
    const auto payload = *payload_or;
    if (payload.size() < 4) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    const std::uint32_t count = read_be32(payload.subspan(0, 4));
    if (payload.size() < 4 + 4ULL * count) {
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    std::vector<std::uint32_t> refs;
    refs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        refs.push_back(read_be32(payload.subspan(4 + 4ULL * i, 4)));
    }
    return refs;
}

}  // namespace merlion_tsdb::block
