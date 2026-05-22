#include "merlion_tsdb/block/meta.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace b = merlion_tsdb::block;

namespace {

// Resolves to the upstream-imported block fixture at testdata/index_format_v1/.
// Walks up from the test binary's CWD looking for the testdata directory.
std::filesystem::path golden_block_dir() {
    auto p = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        const auto candidate = p / "testdata" / "index_format_v1";
        if (std::filesystem::exists(candidate / "meta.json")) return candidate;
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
    ADD_FAILURE() << "could not locate testdata/index_format_v1/ above CWD "
                  << std::filesystem::current_path();
    return {};
}

class TempDir {
public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "merlion_meta_%08x_%08x", rd(), rd());
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

}  // namespace

TEST(BlockMeta, ReadsUpstreamGoldenFixture) {
    auto m = b::read_meta(golden_block_dir());
    ASSERT_TRUE(m.has_value()) << "couldn't read " << golden_block_dir() / "meta.json";
    EXPECT_EQ(m->version, 1);
    EXPECT_EQ(m->ulid, "01DXXFZDYD1MQW6079WK0K6EDQ");
    EXPECT_EQ(m->min_time, 0);
    EXPECT_EQ(m->max_time, 7'200'000);
    EXPECT_EQ(m->stats.num_samples, 102u);
    EXPECT_EQ(m->stats.num_series,  102u);
    EXPECT_EQ(m->stats.num_chunks,  102u);
    EXPECT_EQ(m->compaction.level, 1);
    ASSERT_EQ(m->compaction.sources.size(), 1u);
    EXPECT_EQ(m->compaction.sources[0], "01DXXFZDYD1MQW6079WK0K6EDQ");
    EXPECT_FALSE(m->compaction.deletable);
    EXPECT_TRUE(m->compaction.parents.empty());
    EXPECT_TRUE(m->compaction.hints.empty());
}

TEST(BlockMeta, RoundtripWritesReadable) {
    TempDir tmp;
    b::BlockMeta original;
    original.version  = 1;
    original.ulid     = "01HZ9X8N5K9V2R3T4Y5W6Q7Z8A";
    original.min_time = 1'700'000'000'000;
    original.max_time = 1'700'003'600'000;
    original.stats.num_samples = 12345;
    original.stats.num_series  = 100;
    original.stats.num_chunks  = 250;
    original.compaction.level   = 2;
    original.compaction.sources = {"01ABC...", "01DEF..."};
    original.compaction.parents = {{"01PARENT", 1, 2}, {"01OTHER", 3, 4}};
    original.compaction.hints   = {"from-out-of-order"};

    ASSERT_TRUE(b::write_meta(tmp.path(), original).has_value());

    auto read_back = b::read_meta(tmp.path());
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(*read_back, original);
}

TEST(BlockMeta, AtomicWriteLeavesNoTempFile) {
    TempDir tmp;
    b::BlockMeta m;
    m.version = 1;
    m.ulid = "01HZ_TEST";
    ASSERT_TRUE(b::write_meta(tmp.path(), m).has_value());
    EXPECT_TRUE(std::filesystem::exists(tmp.path() / "meta.json"));
    EXPECT_FALSE(std::filesystem::exists(tmp.path() / "meta.json.tmp"));
}

TEST(BlockMeta, DecodesEmptyOptionalFields) {
    // Most upstream blocks omit zero/empty fields under `omitempty`. The
    // decoder must accept that gracefully.
    const std::string minimal = R"({
        "version": 1,
        "ulid": "01ABC",
        "minTime": 0,
        "maxTime": 100,
        "compaction": {"level": 0}
    })";
    auto m = b::decode_meta_json(minimal);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->ulid, "01ABC");
    EXPECT_EQ(m->stats.num_samples, 0u);
    EXPECT_TRUE(m->compaction.sources.empty());
}

TEST(BlockMeta, RejectsUnknownVersion) {
    const std::string bad = R"({"version": 99, "ulid": "x", "minTime": 0, "maxTime": 0, "compaction": {"level": 0}})";
    auto m = b::decode_meta_json(bad);
    ASSERT_FALSE(m.has_value());
    EXPECT_EQ(m.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(BlockMeta, RejectsMalformedJson) {
    auto m = b::decode_meta_json("{not json}");
    ASSERT_FALSE(m.has_value());
    EXPECT_EQ(m.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(BlockMeta, ReadMissingFileReturnsError) {
    TempDir tmp;
    auto m = b::read_meta(tmp.path());
    EXPECT_FALSE(m.has_value());
}

TEST(BlockMeta, JsonEncodingPreservesAllFieldsOnRoundtrip) {
    b::BlockMeta original;
    original.version  = 1;
    original.ulid     = "01ROUNDTRIP";
    original.min_time = -42;
    original.max_time = 1'000'000;
    original.stats.num_samples = 7;
    original.stats.num_histogram_samples = 3;
    original.stats.num_tombstones = 1;
    original.compaction.level   = 5;
    original.compaction.failed  = true;
    original.compaction.hints   = {"alpha", "beta", "gamma"};

    const auto text = b::encode_meta_json(original);
    auto decoded = b::decode_meta_json(text);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, original);
}
