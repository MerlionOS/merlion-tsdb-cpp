#include "merlion_tsdb/head/head.hpp"

#include <utility>

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

    auto wal = wal::SegmentWriter::open(dir / "wal");
    if (!wal) return std::unexpected(wal.error());

    return Head{dir, std::move(*wal)};
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

std::expected<void, std::error_code> Head::close() {
    if (closed_) return {};
    if (auto r = commit();          !r) { closed_ = true; return r; }
    if (auto r = wal_.cut();        !r) { closed_ = true; return r; }
    closed_ = true;
    return {};
}

}  // namespace merlion_tsdb::head
