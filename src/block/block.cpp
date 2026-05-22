#include "merlion_tsdb/block/block.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "merlion_tsdb/block/index_writer.hpp"
#include "merlion_tsdb/block/ulid.hpp"
#include "merlion_tsdb/head/head.hpp"
#include "merlion_tsdb/head/mem_series.hpp"
#include "merlion_tsdb/head/series_store.hpp"

namespace merlion_tsdb::block {

Block::Block(std::filesystem::path dir,
             BlockMeta meta,
             IndexReader index,
             ChunkReader chunks) noexcept
    : dir_(std::move(dir)),
      meta_(std::move(meta)),
      index_(std::move(index)),
      chunks_(std::move(chunks)) {}

std::expected<Block, std::error_code>
Block::open(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (ec) return std::unexpected(ec);
        return std::unexpected(std::make_error_code(std::errc::not_a_directory));
    }

    auto meta_or = read_meta(dir);
    if (!meta_or) return std::unexpected(meta_or.error());

    auto index_or = IndexReader::open(dir / "index");
    if (!index_or) return std::unexpected(index_or.error());

    auto chunks_or = ChunkReader::open(dir / "chunks");
    if (!chunks_or) return std::unexpected(chunks_or.error());

    return Block{dir,
                 std::move(*meta_or),
                 std::move(*index_or),
                 std::move(*chunks_or)};
}

std::expected<std::filesystem::path, std::error_code>
Block::create_from_series(const std::filesystem::path& parent_dir,
                          std::span<const SeriesInput> series) {
    std::error_code ec;
    std::filesystem::create_directories(parent_dir, ec);
    if (ec) return std::unexpected(ec);

    const auto ulid = new_ulid();
    const auto block_dir = parent_dir / ulid;
    if (std::filesystem::exists(block_dir, ec)) {
        return std::unexpected(std::make_error_code(std::errc::file_exists));
    }
    if (ec) return std::unexpected(ec);
    std::filesystem::create_directories(block_dir, ec);
    if (ec) return std::unexpected(ec);

    // -------- Step 1: write all chunks via ChunkWriter ---------------------
    // For each input series, record the ChunkMeta list with the assigned refs.
    struct PreparedSeries {
        const SeriesInput*           input;
        std::vector<ChunkMeta>       chunk_metas;
    };
    std::vector<PreparedSeries> prepared;
    prepared.reserve(series.size());

    std::uint64_t total_samples = 0;
    std::uint64_t total_chunks  = 0;
    std::int64_t  min_time = std::numeric_limits<std::int64_t>::max();
    std::int64_t  max_time = std::numeric_limits<std::int64_t>::min();

    {
        auto cw_or = ChunkWriter::open(block_dir / "chunks");
        if (!cw_or) return std::unexpected(cw_or.error());
        auto& cw = *cw_or;

        for (const auto& s : series) {
            if (s.chunks.empty()) continue;
            PreparedSeries ps;
            ps.input = &s;
            ps.chunk_metas.reserve(s.chunks.size());

            for (const auto& chk : s.chunks) {
                auto ref = cw.write(chunkenc::Encoding::XOR, chk.bytes);
                if (!ref) return std::unexpected(ref.error());
                ChunkMeta meta;
                meta.min_time = chk.min_time;
                meta.max_time = chk.max_time;
                meta.ref      = ref->to_u64();
                ps.chunk_metas.push_back(meta);

                if (chk.min_time < min_time) min_time = chk.min_time;
                if (chk.max_time > max_time) max_time = chk.max_time;

                // Sample count is encoded in the first 2 bytes of the
                // XOR chunk's body, big-endian.
                if (chk.bytes.size() >= 2) {
                    const std::uint16_t n =
                        static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(chk.bytes[0]) << 8) |
                             static_cast<std::uint16_t>(chk.bytes[1]));
                    total_samples += static_cast<std::uint64_t>(n);
                }
                ++total_chunks;
            }
            prepared.push_back(std::move(ps));
        }
        if (auto r = cw.close(); !r) return std::unexpected(r.error());
    }

    // Empty input is legal; meta.json reflects zero series/samples.
    if (prepared.empty()) {
        min_time = 0;
        max_time = 0;
    }

    // -------- Step 2: build the symbol table -------------------------------
    // Sorted, deduplicated set of every label name + label value plus the
    // empty string (used by upstream's posting offset table for the "all
    // postings" sentinel).
    std::set<std::string> symbols;
    symbols.insert("");
    for (const auto& ps : prepared) {
        for (const auto& l : ps.input->labels.entries()) {
            symbols.insert(l.name);
            symbols.insert(l.value);
        }
    }

    // Map symbol → its IndexWriter ref (0-indexed assignment in sorted order).
    std::unordered_map<std::string, std::uint32_t> sym_ref;
    sym_ref.reserve(symbols.size());

    // -------- Step 3: open IndexWriter and emit -----------------------------
    auto iw_or = IndexWriter::create(block_dir / "index");
    if (!iw_or) return std::unexpected(iw_or.error());
    auto& iw = *iw_or;

    for (const auto& s : symbols) {
        auto ref = iw.add_symbol(s);
        if (!ref) return std::unexpected(ref.error());
        sym_ref.emplace(s, *ref);
    }
    if (auto r = iw.finish_symbols(); !r) return std::unexpected(r.error());

    // Build (name, value) → posting list. Inserting series in the input
    // order assigns ascending series IDs, which keeps the posting lists
    // sorted by id without any extra work.
    std::map<std::pair<std::string, std::string>, std::vector<std::uint32_t>> postings;

    for (auto& ps : prepared) {
        // Translate labels to (name_ref, value_ref) pairs.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> label_refs;
        label_refs.reserve(ps.input->labels.size());
        for (const auto& l : ps.input->labels.entries()) {
            label_refs.emplace_back(sym_ref.at(l.name), sym_ref.at(l.value));
        }
        auto series_id = iw.add_series(label_refs, ps.chunk_metas);
        if (!series_id) return std::unexpected(series_id.error());

        for (const auto& l : ps.input->labels.entries()) {
            postings[{l.name, l.value}].push_back(*series_id);
        }
    }
    if (auto r = iw.finish_series(); !r) return std::unexpected(r.error());

    for (const auto& [key, ids] : postings) {
        if (auto r = iw.add_postings(key.first, key.second, ids); !r) {
            return std::unexpected(r.error());
        }
    }
    if (auto r = iw.close(); !r) return std::unexpected(r.error());

    // -------- Step 4: meta.json --------------------------------------------
    BlockMeta meta;
    meta.version  = 1;
    meta.ulid     = ulid;
    meta.min_time = min_time;
    meta.max_time = max_time;
    meta.stats.num_samples = total_samples;
    meta.stats.num_series  = prepared.size();
    meta.stats.num_chunks  = total_chunks;
    meta.compaction.level   = 1;
    meta.compaction.sources = {ulid};

    if (auto r = write_meta(block_dir, meta); !r) {
        return std::unexpected(r.error());
    }

    // -------- Step 5: empty tombstones file --------------------------------
    {
        std::ofstream out(block_dir / "tombstones", std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    return block_dir;
}

std::expected<std::filesystem::path, std::error_code>
Block::compact(const std::filesystem::path& parent_dir,
               std::span<const std::filesystem::path> input_block_dirs) {
    if (input_block_dirs.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    // -------- Open every input block ---------------------------------------
    std::vector<Block> blocks;
    blocks.reserve(input_block_dirs.size());
    int max_level = 0;
    std::set<std::string> aggregated_sources;
    for (const auto& dir : input_block_dirs) {
        auto blk_or = Block::open(dir);
        if (!blk_or) return std::unexpected(blk_or.error());
        max_level = std::max(max_level, blk_or->meta().compaction.level);
        for (const auto& src : blk_or->meta().compaction.sources) {
            aggregated_sources.insert(src);
        }
        blocks.push_back(std::move(*blk_or));
    }

    // -------- Aggregate every series across every input block --------------
    // Keyed on Labels (canonical form) → list of ChunkInputs collected from
    // every block. The order we collect doesn't matter because we sort by
    // min_time before writing.
    std::unordered_map<model::Labels, std::vector<ChunkInput>, model::LabelsHash>
        merged;

    for (const auto& blk : blocks) {
        // Walk every (name, value) entry in the postings table; collect
        // distinct series IDs; for each ID materialise the series + its
        // chunk metas, then load chunk bytes via the block's ChunkReader.
        std::set<std::uint32_t> seen_ids;
        for (const auto& entry : blk.index().postings_table().entries()) {
            auto refs = read_posting_list(blk.index().bytes(), entry.offset);
            if (!refs) return std::unexpected(refs.error());
            for (auto id : *refs) seen_ids.insert(id);
        }
        for (auto id : seen_ids) {
            auto se = blk.index().series(id);
            if (!se) return std::unexpected(se.error());
            auto& dst = merged[se->labels];
            for (const auto& cm : se->chunks) {
                auto payload = blk.chunks().read(cm.chunk_ref());
                if (!payload) return std::unexpected(payload.error());
                if (payload->encoding != chunkenc::Encoding::XOR) {
                    return std::unexpected(
                        std::make_error_code(std::errc::not_supported));
                }
                ChunkInput ci;
                ci.min_time = cm.min_time;
                ci.max_time = cm.max_time;
                ci.bytes    = std::move(payload->data);
                dst.push_back(std::move(ci));
            }
        }
    }

    // -------- Vertical merge: dedupe samples by timestamp ------------------
    // For each series, decode every input chunk's samples, stable-sort by
    // timestamp (insertion order preserved within ties → later input's
    // value wins on duplicate t), and re-encode into one or more XOR
    // chunks. Re-encoding goes through a throwaway MemSeries so the
    // already-tested head chunk-cutting logic (soft 1 KiB cap, 65 535
    // sample-count cap) takes care of new chunk boundaries.
    std::vector<SeriesInput> series_inputs;
    series_inputs.reserve(merged.size());
    for (auto& [labels, chunks] : merged) {
        // Collect every (t, v) sample from every input chunk.
        std::vector<std::pair<std::int64_t, double>> samples;
        for (const auto& ci : chunks) {
            std::vector<std::uint8_t> copy{ci.bytes.begin(), ci.bytes.end()};
            chunkenc::XORChunk xc = chunkenc::XORChunk::from_bytes(std::move(copy));
            auto it = xc.iterator();
            while (it.next()) samples.emplace_back(it.t(), it.v());
            if (it.error().has_value()) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
        }
        // Stable-sort by t, then collapse ties keeping the last (= latest
        // input's value).
        std::stable_sort(samples.begin(), samples.end(),
                         [](const auto& a, const auto& b) {
                             return a.first < b.first;
                         });
        std::vector<std::pair<std::int64_t, double>> deduped;
        deduped.reserve(samples.size());
        for (std::size_t i = 0; i < samples.size();) {
            std::size_t j = i;
            while (j + 1 < samples.size() &&
                   samples[j + 1].first == samples[i].first) {
                ++j;
            }
            deduped.push_back(samples[j]);
            i = j + 1;
        }

        SeriesInput si;
        si.labels = labels;
        if (!deduped.empty()) {
            head::MemSeries ms(/*ref placeholder*/ 0, labels);
            for (const auto& [t, v] : deduped) {
                if (!ms.append(t, v)) {
                    return std::unexpected(
                        std::make_error_code(std::errc::io_error));
                }
            }
            for (const auto* xc : ms.chunks()) {
                if (xc->num_samples() == 0) continue;
                ChunkInput ci;
                ci.bytes.assign(xc->bytes().begin(), xc->bytes().end());
                auto it = xc->iterator();
                if (!it.next()) continue;
                ci.min_time = it.t();
                ci.max_time = it.t();
                while (it.next()) ci.max_time = it.t();
                si.chunks.push_back(std::move(ci));
            }
        }
        series_inputs.push_back(std::move(si));
    }

    auto dir_or = create_from_series(parent_dir, series_inputs);
    if (!dir_or) return std::unexpected(dir_or.error());

    // -------- Patch meta.json with compaction info -------------------------
    auto meta_or = read_meta(*dir_or);
    if (!meta_or) return std::unexpected(meta_or.error());
    auto meta = std::move(*meta_or);
    meta.compaction.level   = max_level + 1;
    meta.compaction.sources.assign(aggregated_sources.begin(),
                                    aggregated_sources.end());
    if (auto r = write_meta(*dir_or, meta); !r) {
        return std::unexpected(r.error());
    }

    return dir_or;
}

std::expected<std::filesystem::path, std::error_code>
Block::create_from_head(const std::filesystem::path& parent_dir,
                        const head::Head& head) {
    std::vector<SeriesInput> inputs;
    inputs.reserve(head.series().size());

    for (const auto* memseries : head.series().all_series()) {
        if (memseries->num_samples() == 0) continue;

        SeriesInput si;
        si.labels = memseries->labels();
        si.chunks.reserve(memseries->num_chunks());

        for (const auto* xc : memseries->chunks()) {
            if (xc->num_samples() == 0) continue;
            ChunkInput ci;
            ci.bytes.assign(xc->bytes().begin(), xc->bytes().end());

            // Walk the chunk once to recover min_time and max_time.
            // The XOR encoder emits samples in monotonic timestamp
            // order, so the first sample is the min and the last is
            // the max — we can capture them in one pass without
            // tracking running aggregates.
            auto it = xc->iterator();
            if (!it.next()) {
                // Chunk reported num_samples > 0 but iterator yielded
                // nothing. That's a structural mismatch — surface it
                // rather than fabricating bogus times.
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            ci.min_time = it.t();
            ci.max_time = it.t();
            while (it.next()) ci.max_time = it.t();
            if (it.error().has_value()) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            si.chunks.push_back(std::move(ci));
        }

        if (si.chunks.empty()) continue;
        inputs.push_back(std::move(si));
    }

    return create_from_series(parent_dir, inputs);
}

namespace {

// Sorted intersection of two ascending-unique vectors. Output is the
// values present in both inputs, ascending and unique.
std::vector<std::uint32_t>
sorted_intersection(const std::vector<std::uint32_t>& a,
                    const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    out.reserve(std::min(a.size(), b.size()));
    std::set_intersection(a.begin(), a.end(),
                          b.begin(), b.end(),
                          std::back_inserter(out));
    return out;
}

// Sorted difference a \ b. Both inputs must be ascending-unique.
std::vector<std::uint32_t>
sorted_difference(const std::vector<std::uint32_t>& a,
                  const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    out.reserve(a.size());
    std::set_difference(a.begin(), a.end(),
                        b.begin(), b.end(),
                        std::back_inserter(out));
    return out;
}

// Resolve one matcher into its set of matching series ids. Empty-value
// semantics follow Prometheus: a series without label N is treated as
// having N="". For Re/Nre, the matcher's compiled regex is consulted
// per candidate value (Re) or to compute the complement (Nre).
std::expected<std::vector<std::uint32_t>, std::error_code>
postings_for_matcher(const IndexReader& idx, const model::Matcher& m) {
    using model::MatchType;
    switch (m.type()) {
        case MatchType::Eq: {
            if (m.value().empty()) {
                // Series WITHOUT this label (or with empty value, but we
                // never emit empty-value postings).
                auto all = idx.all_postings();
                if (!all) return std::unexpected(all.error());
                auto with = idx.postings_for_name(m.name());
                if (!with) return std::unexpected(with.error());
                return sorted_difference(*all, *with);
            }
            return idx.postings(m.name(), m.value());
        }
        case MatchType::Neq: {
            if (m.value().empty()) {
                // Series WITH this label (any non-empty value).
                return idx.postings_for_name(m.name());
            }
            auto all = idx.all_postings();
            if (!all) return std::unexpected(all.error());
            auto eq  = idx.postings(m.name(), m.value());
            if (!eq) return std::unexpected(eq.error());
            return sorted_difference(*all, *eq);
        }
        case MatchType::Re: {
            std::vector<std::uint32_t> out;
            bool empty_matches = m.matches("");
            // Union every (name, value) whose value matches the regex.
            for (const auto& v : idx.label_values(m.name())) {
                if (!m.matches(v)) continue;
                auto ids = idx.postings(m.name(), v);
                if (!ids) return std::unexpected(ids.error());
                out.insert(out.end(), ids->begin(), ids->end());
            }
            if (empty_matches) {
                // Include series without this label (treated as N="").
                auto all = idx.all_postings();
                if (!all) return std::unexpected(all.error());
                auto with = idx.postings_for_name(m.name());
                if (!with) return std::unexpected(with.error());
                auto without = sorted_difference(*all, *with);
                out.insert(out.end(), without.begin(), without.end());
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }
        case MatchType::Nre: {
            // Compute the set of series the regex DOES match, then
            // subtract from all_postings. Note m.matches() for Nre is
            // already inverted, so we use regex_matches() here to query
            // the underlying pattern.
            std::vector<std::uint32_t> matching;
            const bool empty_matches_regex = m.regex_matches("");
            for (const auto& v : idx.label_values(m.name())) {
                if (!m.regex_matches(v)) continue;
                auto ids = idx.postings(m.name(), v);
                if (!ids) return std::unexpected(ids.error());
                matching.insert(matching.end(), ids->begin(), ids->end());
            }
            if (empty_matches_regex) {
                auto all = idx.all_postings();
                if (!all) return std::unexpected(all.error());
                auto with = idx.postings_for_name(m.name());
                if (!with) return std::unexpected(with.error());
                auto without = sorted_difference(*all, *with);
                matching.insert(matching.end(), without.begin(), without.end());
            }
            std::sort(matching.begin(), matching.end());
            matching.erase(std::unique(matching.begin(), matching.end()),
                           matching.end());
            auto all = idx.all_postings();
            if (!all) return std::unexpected(all.error());
            return sorted_difference(*all, matching);
        }
    }
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

}  // namespace

std::expected<std::vector<Block::QueryResult>, std::error_code>
Block::select(std::span<const model::Matcher> matchers,
              std::int64_t mint,
              std::int64_t maxt) const {
    if (matchers.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    // 1. Resolve each matcher to its posting list, intersect from the
    //    smallest outward to minimize work. Sort matchers by initial
    //    resolution size? Skip the optimization for MVP — resolve in
    //    caller order and rely on sorted_intersection being O(n+m).
    std::vector<std::uint32_t> ids;
    {
        auto first = postings_for_matcher(index_, matchers[0]);
        if (!first) return std::unexpected(first.error());
        ids = std::move(*first);
    }
    for (std::size_t i = 1; i < matchers.size() && !ids.empty(); ++i) {
        auto next = postings_for_matcher(index_, matchers[i]);
        if (!next) return std::unexpected(next.error());
        ids = sorted_intersection(ids, *next);
    }

    // 2. Decode each surviving series, filter chunks by time range.
    std::vector<QueryResult> out;
    out.reserve(ids.size());
    for (auto id : ids) {
        auto series_or = index_.series(id);
        if (!series_or) return std::unexpected(series_or.error());

        QueryResult qr;
        qr.labels = std::move(series_or->labels);
        for (auto& cm : series_or->chunks) {
            // Half-open overlap test: chunk [cm.min_time, cm.max_time]
            // intersects query [mint, maxt] iff cm.min_time <= maxt &&
            // cm.max_time >= mint. Upstream uses inclusive bounds on
            // both sides; we follow.
            if (cm.min_time > maxt || cm.max_time < mint) continue;
            auto payload = chunks_.read(cm.chunk_ref());
            if (!payload) return std::unexpected(payload.error());
            if (payload->encoding != chunkenc::Encoding::XOR) {
                return std::unexpected(std::make_error_code(std::errc::not_supported));
            }
            qr.chunks.push_back(
                chunkenc::XORChunk::from_bytes(std::move(payload->data)));
            qr.chunk_metas.push_back(cm);
        }
        // Drop series whose every chunk was outside the time range.
        if (qr.chunks.empty()) continue;
        out.push_back(std::move(qr));
    }
    return out;
}

std::expected<std::vector<Block::QueryResult>, std::error_code>
Block::query(std::string_view name, std::string_view value) const {
    auto ids_or = index_.postings(name, value);
    if (!ids_or) return std::unexpected(ids_or.error());

    std::vector<QueryResult> out;
    out.reserve(ids_or->size());
    for (auto id : *ids_or) {
        auto series_or = index_.series(id);
        if (!series_or) return std::unexpected(series_or.error());

        QueryResult qr;
        qr.labels      = std::move(series_or->labels);
        qr.chunk_metas = std::move(series_or->chunks);
        qr.chunks.reserve(qr.chunk_metas.size());
        for (const auto& cm : qr.chunk_metas) {
            auto payload = chunks_.read(cm.chunk_ref());
            if (!payload) return std::unexpected(payload.error());
            // For MVP only XOR-encoded chunks are decoded; histogram /
            // float-histogram chunks can be added in a follow-up that
            // ports those encoders.
            if (payload->encoding != chunkenc::Encoding::XOR) {
                return std::unexpected(std::make_error_code(std::errc::not_supported));
            }
            qr.chunks.push_back(
                chunkenc::XORChunk::from_bytes(std::move(payload->data)));
        }
        out.push_back(std::move(qr));
    }
    return out;
}

}  // namespace merlion_tsdb::block
