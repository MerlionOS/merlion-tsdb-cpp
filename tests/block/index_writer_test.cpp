#include "merlion_tsdb/block/index_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/block/index.hpp"

namespace b = merlion_tsdb::block;

namespace {

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "merlion_idxw_%08x_%08x", rd(), rd());
        path_ = base / buf;
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

// Writes a minimal valid index (empty symbol table, no series, no
// postings) to verify the framing scaffolding without exercising the
// payloads.
void write_empty_index(const std::filesystem::path& file) {
    auto w = *b::IndexWriter::create(file);
    ASSERT_TRUE(w.finish_symbols().has_value());
    ASSERT_TRUE(w.finish_series().has_value());
    ASSERT_TRUE(w.close().has_value());
}

}  // namespace

TEST(IndexWriter, RefusesToOverwriteExistingFile) {
    TempDir tmp;
    const auto file = tmp.path() / "index";
    std::ofstream(file, std::ios::binary) << "preexisting";
    auto w = b::IndexWriter::create(file);
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error(), std::make_error_code(std::errc::file_exists));
}

TEST(IndexWriter, EmptyIndexRoundtripsThroughReader) {
    TempDir tmp;
    const auto file = tmp.path() / "index";
    write_empty_index(file);

    auto rdr = b::IndexReader::open(file);
    ASSERT_TRUE(rdr.has_value());
    EXPECT_EQ(rdr->version(), b::k_index_write_format);
    EXPECT_EQ(rdr->symbols().count(), 0u);
    EXPECT_EQ(rdr->postings_table().size(), 0u);
}

TEST(IndexWriter, RejectsOutOfOrderSymbolAdds) {
    TempDir tmp;
    auto w = *b::IndexWriter::create(tmp.path() / "index");
    ASSERT_TRUE(w.add_symbol("abc").has_value());
    auto bad = w.add_symbol("aaa");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(IndexWriter, RejectsOutOfOrderPostingIds) {
    TempDir tmp;
    auto w = *b::IndexWriter::create(tmp.path() / "index");
    ASSERT_TRUE(w.finish_symbols().has_value());
    ASSERT_TRUE(w.finish_series().has_value());
    const std::vector<std::uint32_t> ids{5, 3, 10};  // not ascending
    auto bad = w.add_postings("name", "value", ids);
    ASSERT_FALSE(bad.has_value());
}

TEST(IndexWriter, EnforcesPhaseOrder) {
    TempDir tmp;
    auto w = *b::IndexWriter::create(tmp.path() / "index");
    // Can't add a series before finishing symbols.
    const std::pair<std::uint32_t, std::uint32_t> lr{0, 0};
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 1> labels{lr};
    const std::array<b::ChunkMeta, 0> chks{};
    auto early = w.add_series(labels, chks);
    ASSERT_FALSE(early.has_value());
}

TEST(IndexWriter, SymbolTableReadback) {
    TempDir tmp;
    const auto file = tmp.path() / "index";
    {
        auto w = *b::IndexWriter::create(file);
        // Symbols must be in strictly ascending lex order.
        for (const auto* s : {"", "a", "bar", "baz", "foo", "value0", "value1"}) {
            ASSERT_TRUE(w.add_symbol(s).has_value()) << s;
        }
        ASSERT_TRUE(w.finish_symbols().has_value());
        ASSERT_TRUE(w.finish_series().has_value());
        ASSERT_TRUE(w.close().has_value());
    }
    auto rdr = *b::IndexReader::open(file);
    const auto symbols = rdr.symbols().all_symbols();
    EXPECT_EQ(symbols, (std::vector<std::string>{"", "a", "bar", "baz", "foo", "value0", "value1"}));
}

TEST(IndexWriter, SingleSeriesRoundtrip) {
    TempDir tmp;
    const auto file = tmp.path() / "index";
    std::uint32_t series_id = 0;
    {
        auto w = *b::IndexWriter::create(file);
        // Symbols: "" (empty), "__name__", "instance", "metric", "self".
        // Strictly ascending lex order.
        ASSERT_TRUE(w.add_symbol("").has_value());
        const auto name_ref     = *w.add_symbol("__name__");
        const auto instance_ref = *w.add_symbol("instance");
        const auto metric_ref   = *w.add_symbol("metric");
        const auto self_ref     = *w.add_symbol("self");
        ASSERT_TRUE(w.finish_symbols().has_value());

        const std::array<std::pair<std::uint32_t, std::uint32_t>, 2> labels{{
            {name_ref,     metric_ref},
            {instance_ref, self_ref},
        }};
        const std::array<b::ChunkMeta, 1> chks{{
            {.min_time = 1000, .max_time = 2000, .ref = 0x100000008ULL},
        }};
        auto id = w.add_series(labels, chks);
        ASSERT_TRUE(id.has_value());
        series_id = *id;
        ASSERT_TRUE(w.finish_series().has_value());

        const std::array<std::uint32_t, 1> ids{series_id};
        ASSERT_TRUE(w.add_postings("__name__", "metric", ids).has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    auto rdr = *b::IndexReader::open(file);
    EXPECT_EQ(rdr.version(), b::k_index_write_format);
    EXPECT_EQ(rdr.symbols().count(), 5u);

    // The posting must come back.
    auto refs = rdr.postings("__name__", "metric");
    ASSERT_TRUE(refs.has_value());
    ASSERT_EQ(refs->size(), 1u);
    EXPECT_EQ((*refs)[0], series_id);

    // The series must decode with the correct labels + chunk meta.
    auto entry = rdr.series(series_id);
    ASSERT_TRUE(entry.has_value()) << "series read failed";
    ASSERT_EQ(entry->labels.size(), 2u);
    EXPECT_EQ(entry->labels.get("__name__"), std::optional<std::string_view>{"metric"});
    EXPECT_EQ(entry->labels.get("instance"), std::optional<std::string_view>{"self"});
    ASSERT_EQ(entry->chunks.size(), 1u);
    EXPECT_EQ(entry->chunks[0].min_time, 1000);
    EXPECT_EQ(entry->chunks[0].max_time, 2000);
    EXPECT_EQ(entry->chunks[0].ref,      0x100000008ULL);
}

TEST(IndexWriter, MultipleSeriesAndPostingsRoundtrip) {
    TempDir tmp;
    const auto file = tmp.path() / "index";
    std::vector<std::uint32_t> ids;
    {
        auto w = *b::IndexWriter::create(file);
        ASSERT_TRUE(w.add_symbol("").has_value());
        const auto bar_ref  = *w.add_symbol("bar");
        const auto foo_ref  = *w.add_symbol("foo");
        const auto v0_ref   = *w.add_symbol("v0");
        const auto v1_ref   = *w.add_symbol("v1");
        const auto v2_ref   = *w.add_symbol("v2");
        ASSERT_TRUE(w.finish_symbols().has_value());

        // 3 series, each with one chunk in increasing time ranges.
        // Series 0: {foo=v0}        chunk [0..1000]
        // Series 1: {foo=v1, bar=v2} chunk [0..2000]
        // Series 2: {foo=v2}        chunk [0..3000]
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 1> l0{{{foo_ref, v0_ref}}};
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 2> l1{{
            {bar_ref, v2_ref}, {foo_ref, v1_ref},
        }};
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 1> l2{{{foo_ref, v2_ref}}};

        const std::array<b::ChunkMeta, 1> c0{{{.min_time = 0, .max_time = 1000, .ref = 0x100000008ULL}}};
        const std::array<b::ChunkMeta, 1> c1{{{.min_time = 0, .max_time = 2000, .ref = 0x100000020ULL}}};
        const std::array<b::ChunkMeta, 1> c2{{{.min_time = 0, .max_time = 3000, .ref = 0x100000038ULL}}};

        ids.push_back(*w.add_series(l0, c0));
        ids.push_back(*w.add_series(l1, c1));
        ids.push_back(*w.add_series(l2, c2));
        ASSERT_TRUE(w.finish_series().has_value());

        // ids are ascending by construction.
        const std::array<std::uint32_t, 1> p_foo_v0{ids[0]};
        const std::array<std::uint32_t, 1> p_foo_v1{ids[1]};
        const std::array<std::uint32_t, 1> p_foo_v2{ids[2]};
        const std::array<std::uint32_t, 1> p_bar_v2{ids[1]};
        ASSERT_TRUE(w.add_postings("foo", "v0", p_foo_v0).has_value());
        ASSERT_TRUE(w.add_postings("foo", "v1", p_foo_v1).has_value());
        ASSERT_TRUE(w.add_postings("foo", "v2", p_foo_v2).has_value());
        ASSERT_TRUE(w.add_postings("bar", "v2", p_bar_v2).has_value());
        ASSERT_TRUE(w.close().has_value());
    }

    auto rdr = *b::IndexReader::open(file);
    EXPECT_EQ(rdr.symbols().count(), 6u);
    EXPECT_EQ(rdr.postings_table().size(), 4u);

    auto recover = [&](std::string_view n, std::string_view v) {
        auto r = rdr.postings(n, v);
        EXPECT_TRUE(r.has_value());
        return r.value_or(std::vector<std::uint32_t>{});
    };
    EXPECT_EQ(recover("foo", "v0"), std::vector<std::uint32_t>{ids[0]});
    EXPECT_EQ(recover("foo", "v1"), std::vector<std::uint32_t>{ids[1]});
    EXPECT_EQ(recover("foo", "v2"), std::vector<std::uint32_t>{ids[2]});
    EXPECT_EQ(recover("bar", "v2"), std::vector<std::uint32_t>{ids[1]});
    EXPECT_TRUE(recover("foo", "missing").empty());

    // Decode each series and confirm labels.
    auto s0 = *rdr.series(ids[0]);
    EXPECT_EQ(s0.labels.size(), 1u);
    EXPECT_EQ(s0.labels.get("foo"), std::optional<std::string_view>{"v0"});

    auto s1 = *rdr.series(ids[1]);
    EXPECT_EQ(s1.labels.size(), 2u);
    EXPECT_EQ(s1.labels.get("foo"), std::optional<std::string_view>{"v1"});
    EXPECT_EQ(s1.labels.get("bar"), std::optional<std::string_view>{"v2"});

    auto s2 = *rdr.series(ids[2]);
    EXPECT_EQ(s2.labels.size(), 1u);
    EXPECT_EQ(s2.labels.get("foo"), std::optional<std::string_view>{"v2"});
}

TEST(IndexWriter, SeriesIdsAreSixteenByteAligned) {
    TempDir tmp;
    const auto file = tmp.path() / "index";
    std::vector<std::uint32_t> ids;
    {
        auto w = *b::IndexWriter::create(file);
        ASSERT_TRUE(w.add_symbol("").has_value());
        const auto a = *w.add_symbol("a");
        const auto b = *w.add_symbol("b");
        ASSERT_TRUE(w.finish_symbols().has_value());

        const std::array<std::pair<std::uint32_t, std::uint32_t>, 1> labels{{{a, b}}};
        const std::array<b::ChunkMeta, 1> chks{{
            {.min_time = 0, .max_time = 100, .ref = 0x100000008ULL},
        }};
        for (int i = 0; i < 5; ++i) {
            ids.push_back(*w.add_series(labels, chks));
        }
        ASSERT_TRUE(w.finish_series().has_value());
        ASSERT_TRUE(w.close().has_value());
    }
    // Each consecutive series ID must differ by at least 1 (since each
    // series, after padding to 16, occupies one or more 16-byte slots).
    for (std::size_t i = 1; i < ids.size(); ++i) {
        EXPECT_GT(ids[i], ids[i - 1]);
    }
}
