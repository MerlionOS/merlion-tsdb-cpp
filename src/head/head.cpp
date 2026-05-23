#include "merlion_tsdb/head/head.hpp"

#include <utility>

#include "merlion_tsdb/wal/segment_reader.hpp"

namespace merlion_tsdb::head {

namespace {

// Convert in-memory Labels → wal::record::Label vector for emission.
// Cheap copy; both sides use std::string.
std::vector<wal::record::Label> to_wal_labels(const model::Labels& l) {
    std::vector<wal::record::Label> out;
    out.reserve(l.size());
    for (const auto& e : l.entries()) {
        out.push_back({.name = e.name, .value = e.value});
    }
    return out;
}

// Reverse direction: wal::record::Label vector → model::Labels (canonical).
model::Labels to_model_labels(const std::vector<wal::record::Label>& src) {
    std::vector<model::Label> entries;
    entries.reserve(src.size());
    for (const auto& l : src) entries.push_back({l.name, l.value});
    return model::Labels{std::move(entries)};
}

// Walk every record in `wal_dir`, reconstructing the in-memory series map
// and re-appending samples to MemSeries chunks. A non-existent directory
// is treated as a fresh head (no replay needed). A torn record at the
// tail of the last segment is dropped silently (matches Go semantics);
// any other read error is fatal.
std::expected<void, std::error_code>
replay_wal_into(const std::filesystem::path& wal_dir, SeriesStore& store) {
    auto rdr_or = wal::SegmentReader::open(wal_dir);
    if (!rdr_or) {
        const auto& ec = rdr_or.error();
        // Empty / missing wal directory means nothing to replay.
        if (ec == std::make_error_code(std::errc::no_such_file_or_directory) ||
            ec == std::make_error_code(std::errc::not_a_directory)) {
            return {};
        }
        return std::unexpected(ec);
    }
    auto& rdr = *rdr_or;

    while (true) {
        auto rec = rdr.next();
        if (!rec) {
            if (rec.error() == wal::SegmentReadError::EndOfStream) return {};
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }

        const auto type = wal::record::peek_type(*rec);
        if (type == wal::record::Type::Series) {
            auto decoded = wal::record::decode_series(*rec);
            if (!decoded) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            for (const auto& s : *decoded) {
                if (!store.insert_with_ref(s.ref, to_model_labels(s.labels))) {
                    return std::unexpected(std::make_error_code(std::errc::io_error));
                }
            }
        } else if (type == wal::record::Type::SamplesV2) {
            auto decoded = wal::record::decode_samples_v2(*rec);
            if (!decoded) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            for (const auto& s : *decoded) {
                auto* series = store.get(s.ref);
                if (!series) {
                    // Sample referencing an unknown series. Either the WAL
                    // is corrupt or it was written by a version that
                    // omitted the matching Series record. Bail.
                    return std::unexpected(std::make_error_code(std::errc::io_error));
                }
                if (!series->append(s.t, s.v)) {
                    return std::unexpected(std::make_error_code(std::errc::io_error));
                }
            }
        }
        // Other record types (Samples V1, Tombstones, Exemplars,
        // histograms, …) aren't emitted by this Head yet; silently skip
        // them for forward compatibility against future writers.
    }
}

}  // namespace

Head::Head(std::filesystem::path dir, wal::SegmentWriter wal) noexcept
    : dir_(std::move(dir)), wal_(std::move(wal)) {}

Head::Head(Head&& other) noexcept
    : dir_(std::move(other.dir_)),
      wal_(std::move(other.wal_)),
      series_(std::move(other.series_)),
      pending_series_(std::move(other.pending_series_)),
      pending_samples_(std::move(other.pending_samples_)),
      closed_(other.closed_) {
    other.closed_ = true;  // moved-from is treated as closed
}

Head& Head::operator=(Head&& other) noexcept {
    if (this != &other) {
        // Best-effort: don't lose data on assignment. Caller really should
        // have closed first; if they didn't, swallow any commit error.
        if (!closed_) {
            (void)commit();
        }
        dir_             = std::move(other.dir_);
        wal_             = std::move(other.wal_);
        series_          = std::move(other.series_);
        pending_series_  = std::move(other.pending_series_);
        pending_samples_ = std::move(other.pending_samples_);
        closed_          = other.closed_;
        other.closed_    = true;
    }
    return *this;
}

Head::~Head() {
    if (!closed_) {
        (void)close();
    }
}

std::expected<Head, std::error_code>
Head::open(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return std::unexpected(ec);

    // Replay existing WAL first, BEFORE opening the segment writer (which
    // would otherwise create a new empty segment and inflate the replay
    // path with a no-op file). After replay, the writer opens at the next
    // free index for fresh appends.
    SeriesStore store;
    if (auto r = replay_wal_into(dir / "wal", store); !r) {
        return std::unexpected(r.error());
    }

    auto wal = wal::SegmentWriter::open(dir / "wal");
    if (!wal) return std::unexpected(wal.error());

    Head h{dir, std::move(*wal)};
    h.series_ = std::move(store);
    return h;
}

std::expected<SeriesRef, std::error_code>
Head::append(const model::Labels& labels, std::int64_t t, double v) {
    if (closed_) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    auto [series, fresh] = series_.get_or_create(labels);
    if (fresh) {
        pending_series_.push_back(
            wal::record::RefSeries{.ref = series->ref(), .labels = to_wal_labels(labels)});
    }

    if (!series->append(t, v)) {
        // Most likely: out-of-order timestamp. Surface as a domain error.
        return std::unexpected(
            std::make_error_code(std::errc::invalid_argument));
    }

    pending_samples_.push_back(
        wal::record::RefSample{.ref = series->ref(), .t = t, .st = 0, .v = v});
    return series->ref();
}

std::expected<void, std::error_code> Head::commit() {
    if (closed_) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    if (!pending_series_.empty()) {
        auto rec = wal::record::encode_series(pending_series_);
        if (auto r = wal_.log(rec); !r) return std::unexpected(r.error());
        pending_series_.clear();
    }
    if (!pending_samples_.empty()) {
        auto rec = wal::record::encode_samples_v2(pending_samples_);
        if (auto r = wal_.log(rec); !r) return std::unexpected(r.error());
        pending_samples_.clear();
    }
    // fsync so the records are durable on return. Without this, the OS
    // page cache could lose them in a crash even though our write() call
    // returned success.
    return wal_.sync();
}

namespace {

// True iff every matcher accepts `labels`. A label that's absent is
// treated as having the empty-string value — same semantics as
// Block::select (see SPEC §8.2).
bool labels_match_all(const model::Labels& labels,
                      std::span<const model::Matcher> matchers) {
    for (const auto& m : matchers) {
        const auto v = labels.get(m.name());
        if (!m.matches(v.value_or(""))) return false;
    }
    return true;
}

}  // namespace

std::expected<std::vector<Head::QueryResult>, std::error_code>
Head::select(std::span<const model::Matcher> matchers,
             std::int64_t mint,
             std::int64_t maxt) const {
    if (matchers.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    std::vector<QueryResult> out;
    for (const auto* s : series_.all_series()) {
        if (s == nullptr || s->num_samples() == 0) continue;
        if (!labels_match_all(s->labels(), matchers)) continue;

        QueryResult qr;
        qr.labels = s->labels();

        for (const auto* xc : s->chunks()) {
            if (xc == nullptr || xc->num_samples() == 0) continue;

            // Iterate once on the live chunk to recover (min, max). XOR
            // emits samples in monotonic-t order so first = min, last =
            // max. We do this BEFORE copying the bytes so we can skip
            // non-overlapping chunks without paying for an allocation.
            auto it = xc->iterator();
            if (!it.next()) continue;  // structurally empty
            const std::int64_t cmin = it.t();
            std::int64_t cmax = cmin;
            while (it.next()) cmax = it.t();
            if (it.error().has_value()) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            // Inclusive-on-both-ends overlap test, matching §8.1.
            if (cmin > maxt || cmax < mint) continue;

            // Snapshot the chunk bytes — MemSeries may still be appended
            // to after this call returns, so the caller must own its
            // own copy.
            std::vector<std::uint8_t> bytes(xc->bytes().begin(), xc->bytes().end());
            qr.chunks.push_back(chunkenc::XORChunk::from_bytes(std::move(bytes)));

            block::ChunkMeta cm;
            cm.min_time = cmin;
            cm.max_time = cmax;
            cm.ref      = 0;  // in-memory chunks have no on-disk ref
            qr.chunk_metas.push_back(cm);
        }

        if (qr.chunks.empty()) continue;
        out.push_back(std::move(qr));
    }
    return out;
}

std::expected<void, std::error_code> Head::close() {
    if (closed_) return {};
    if (auto r = commit();          !r) { closed_ = true; return r; }
    if (auto r = wal_.cut();        !r) { closed_ = true; return r; }
    closed_ = true;
    return {};
}

}  // namespace merlion_tsdb::head
