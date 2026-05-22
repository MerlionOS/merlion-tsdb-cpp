#include "merlion_tsdb/block/meta.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace merlion_tsdb::block {

namespace {

using json = nlohmann::json;

std::error_code errno_ec() noexcept {
    return {errno, std::generic_category()};
}

// Populate `dst` from `src`. Missing fields stay at their default. This
// is more forgiving than nlohmann's strict get<T>() — upstream meta.json
// omits zero-valued fields under `omitempty`.
template <typename T>
void load_if_present(const json& src, const char* key, T& dst) {
    if (auto it = src.find(key); it != src.end() && !it->is_null()) {
        dst = it->get<T>();
    }
}

void load_stats(const json& src, BlockStats& dst) {
    load_if_present(src, "numSamples",          dst.num_samples);
    load_if_present(src, "numFloatSamples",     dst.num_float_samples);
    load_if_present(src, "numHistogramSamples", dst.num_histogram_samples);
    load_if_present(src, "numSeries",           dst.num_series);
    load_if_present(src, "numChunks",           dst.num_chunks);
    load_if_present(src, "numTombstones",       dst.num_tombstones);
}

void dump_stats(json& dst, const BlockStats& s) {
    if (s.num_samples)           dst["numSamples"]          = s.num_samples;
    if (s.num_float_samples)     dst["numFloatSamples"]     = s.num_float_samples;
    if (s.num_histogram_samples) dst["numHistogramSamples"] = s.num_histogram_samples;
    if (s.num_series)            dst["numSeries"]           = s.num_series;
    if (s.num_chunks)            dst["numChunks"]           = s.num_chunks;
    if (s.num_tombstones)        dst["numTombstones"]       = s.num_tombstones;
}

void load_block_desc(const json& src, BlockDesc& dst) {
    load_if_present(src, "ulid",    dst.ulid);
    load_if_present(src, "minTime", dst.min_time);
    load_if_present(src, "maxTime", dst.max_time);
}

void dump_block_desc(json& dst, const BlockDesc& d) {
    dst["ulid"]    = d.ulid;
    dst["minTime"] = d.min_time;
    dst["maxTime"] = d.max_time;
}

void load_compaction(const json& src, BlockMetaCompaction& dst) {
    load_if_present(src, "level",     dst.level);
    load_if_present(src, "sources",   dst.sources);
    load_if_present(src, "deletable", dst.deletable);
    load_if_present(src, "failed",    dst.failed);
    load_if_present(src, "hints",     dst.hints);

    if (auto it = src.find("parents"); it != src.end() && it->is_array()) {
        dst.parents.clear();
        dst.parents.reserve(it->size());
        for (const auto& p : *it) {
            BlockDesc d;
            load_block_desc(p, d);
            dst.parents.push_back(std::move(d));
        }
    }
}

void dump_compaction(json& dst, const BlockMetaCompaction& c) {
    dst["level"] = c.level;
    if (!c.sources.empty())   dst["sources"]   = c.sources;
    if (c.deletable)          dst["deletable"] = true;
    if (!c.parents.empty()) {
        json arr = json::array();
        for (const auto& p : c.parents) {
            json o;
            dump_block_desc(o, p);
            arr.push_back(std::move(o));
        }
        dst["parents"] = std::move(arr);
    }
    if (c.failed)             dst["failed"]    = true;
    if (!c.hints.empty())     dst["hints"]     = c.hints;
}

std::expected<BlockMeta, std::error_code> from_json(const json& root) noexcept {
    BlockMeta m;
    try {
        load_if_present(root, "version", m.version);
        if (m.version != k_meta_version_1) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        load_if_present(root, "ulid",    m.ulid);
        load_if_present(root, "minTime", m.min_time);
        load_if_present(root, "maxTime", m.max_time);
        if (auto it = root.find("stats"); it != root.end() && it->is_object()) {
            load_stats(*it, m.stats);
        }
        if (auto it = root.find("compaction"); it != root.end() && it->is_object()) {
            load_compaction(*it, m.compaction);
        }
    } catch (const json::exception&) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    return m;
}

json to_json(const BlockMeta& m) {
    json root;
    // Field order matches upstream's struct declaration for deterministic
    // diffs. nlohmann::json preserves insertion order.
    root["ulid"]    = m.ulid;
    root["minTime"] = m.min_time;
    root["maxTime"] = m.max_time;

    json stats_obj = json::object();
    dump_stats(stats_obj, m.stats);
    if (!stats_obj.empty()) root["stats"] = std::move(stats_obj);

    json compact_obj = json::object();
    dump_compaction(compact_obj, m.compaction);
    root["compaction"] = std::move(compact_obj);

    root["version"] = m.version;
    return root;
}

std::expected<void, std::error_code>
atomic_write(const std::filesystem::path& path, std::string_view content) {
    const auto tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(errno_ec());
        out.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
        if (!out) return std::unexpected(errno_ec());
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) return std::unexpected(ec);

    // Best-effort directory fsync so the rename is durable. Errors are
    // ignored — fsync of a directory isn't portable on all filesystems.
    if (const int dir_fd = ::open(path.parent_path().c_str(),
                                  O_RDONLY | O_CLOEXEC);
        dir_fd >= 0) {
        (void)::fsync(dir_fd);
        ::close(dir_fd);
    }
    return {};
}

}  // namespace

std::expected<BlockMeta, std::error_code>
read_meta(const std::filesystem::path& dir) {
    const auto path = dir / k_meta_filename;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(errno_ec());
    std::stringstream ss;
    ss << in.rdbuf();
    return decode_meta_json(ss.str());
}

std::expected<void, std::error_code>
write_meta(const std::filesystem::path& dir, const BlockMeta& meta) {
    return atomic_write(dir / k_meta_filename, encode_meta_json(meta));
}

std::string encode_meta_json(const BlockMeta& meta) {
    return to_json(meta).dump(/*indent*/ 4);
}

std::expected<BlockMeta, std::error_code>
decode_meta_json(std::string_view json_text) {
    try {
        const auto root = json::parse(json_text);
        return from_json(root);
    } catch (const json::exception&) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
}

}  // namespace merlion_tsdb::block
