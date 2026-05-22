#include "merlion_tsdb/block/chunks.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "merlion_tsdb/chunkenc/xor.hpp"

namespace b = merlion_tsdb::block;
namespace c = merlion_tsdb::chunkenc;

namespace {

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
        std::snprintf(buf, sizeof(buf), "merlion_chunks_%08x_%08x", rd(), rd());
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

// Build a small XOR chunk holding the given samples and return its bytes.
std::vector<std::uint8_t> make_xor_chunk_bytes(
    std::initializer_list<std::pair<std::int64_t, double>> samples) {
    c::XORChunk chunk;
    auto app = chunk.appender();
    EXPECT_TRUE(app.has_value());
    for (auto [t, v] : samples) EXPECT_TRUE(app->append(t, v));
    return {chunk.bytes().begin(), chunk.bytes().end()};
}

}  // namespace

TEST(ChunkRef, PackingRoundtrips) {
    b::BlockChunkRef r{0x12345678U, 0x9ABCDEF0U};
    auto packed = r.to_u64();
    EXPECT_EQ(packed, 0x123456789ABCDEF0ULL);
    auto back = b::BlockChunkRef::from_u64(packed);
    EXPECT_EQ(back, r);
}

TEST(ChunkWriter, SegmentStartsAtOne) {
    TempDir tmp;
    auto w = *b::ChunkWriter::open(tmp.path() / "chunks");
    EXPECT_EQ(w.current_seq(), 1u);
    EXPECT_TRUE(std::filesystem::exists(tmp.path() / "chunks" / "000001"));
}

TEST(ChunkWriter, HeaderHasCorrectMagicAndVersion) {
    TempDir tmp;
    auto w = *b::ChunkWriter::open(tmp.path() / "chunks");
    ASSERT_TRUE(w.close().has_value());

    std::ifstream in(tmp.path() / "chunks" / "000001", std::ios::binary);
    std::vector<std::uint8_t> header(b::k_chunks_segment_header_size);
    in.read(reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
    EXPECT_EQ(header[0], 0x85);
    EXPECT_EQ(header[1], 0xBD);
    EXPECT_EQ(header[2], 0x40);
    EXPECT_EQ(header[3], 0xDD);
    EXPECT_EQ(header[4], 1);  // version
    EXPECT_EQ(header[5], 0);
    EXPECT_EQ(header[6], 0);
    EXPECT_EQ(header[7], 0);
}

TEST(ChunkRoundtrip, SingleChunkWriteThenRead) {
    TempDir tmp;
    const auto data = make_xor_chunk_bytes({{100, 1.0}, {200, 2.0}, {300, 3.0}});

    auto w = *b::ChunkWriter::open(tmp.path() / "chunks");
    auto ref = w.write(c::Encoding::XOR, data);
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->seq, 1u);
    EXPECT_EQ(ref->offset, b::k_chunks_segment_header_size);
    ASSERT_TRUE(w.close().has_value());

    auto rdr = *b::ChunkReader::open(tmp.path() / "chunks");
    auto payload = rdr.read(*ref);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->encoding, c::Encoding::XOR);
    EXPECT_EQ(payload->data, data);
}

TEST(ChunkRoundtrip, ManyChunksAcrossSegmentBoundary) {
    TempDir tmp;
    // Tiny segment so we hit multiple files.
    auto w = *b::ChunkWriter::open(tmp.path() / "chunks", /*segment_size*/ 2048);
    std::vector<std::pair<b::BlockChunkRef, std::vector<std::uint8_t>>> written;
    std::mt19937_64 rng(0xC0FFEE);
    for (int i = 0; i < 50; ++i) {
        c::XORChunk chunk;
        auto app = chunk.appender();
        for (int j = 0; j < 20; ++j) {
            ASSERT_TRUE(app->append(j * 1000, std::bit_cast<double>(rng())));
        }
        std::vector<std::uint8_t> data(chunk.bytes().begin(), chunk.bytes().end());
        auto ref = w.write(c::Encoding::XOR, data);
        ASSERT_TRUE(ref.has_value()) << "i=" << i;
        written.emplace_back(*ref, std::move(data));
    }
    ASSERT_TRUE(w.close().has_value());

    auto rdr = *b::ChunkReader::open(tmp.path() / "chunks");
    EXPECT_GE(rdr.segment_count(), 2u) << "test should span multiple segments";
    for (const auto& [ref, data] : written) {
        auto p = rdr.read(ref);
        ASSERT_TRUE(p.has_value()) << "ref=(" << ref.seq << "," << ref.offset << ")";
        EXPECT_EQ(p->encoding, c::Encoding::XOR);
        EXPECT_EQ(p->data, data);
    }
}

TEST(ChunkReader, ResumesAtNextSeqAfterExistingFiles) {
    TempDir tmp;
    // Pre-create a placeholder file at seq=5.
    std::filesystem::create_directories(tmp.path() / "chunks");
    {
        std::ofstream(tmp.path() / "chunks" / "000005") << "ignored";
    }
    // Writer should resume at 6.
    auto w = *b::ChunkWriter::open(tmp.path() / "chunks");
    EXPECT_EQ(w.current_seq(), 6u);
    EXPECT_TRUE(std::filesystem::exists(tmp.path() / "chunks" / "000006"));
}

TEST(ChunkReader, CrcCorruptionIsDetected) {
    TempDir tmp;
    const auto data = make_xor_chunk_bytes({{0, 1.0}, {1000, 2.0}});
    auto w = *b::ChunkWriter::open(tmp.path() / "chunks");
    auto ref = *w.write(c::Encoding::XOR, data);
    ASSERT_TRUE(w.close().has_value());

    // Flip a CRC byte.
    const auto seg = tmp.path() / "chunks" / "000001";
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(seg, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    bytes.back() ^= 0x01;
    {
        std::ofstream out(seg, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    auto rdr = *b::ChunkReader::open(tmp.path() / "chunks");
    auto r = rdr.read(ref);
    EXPECT_FALSE(r.has_value());
}

TEST(ChunkReader, IterateRecoversAllChunks) {
    TempDir tmp;
    auto w = *b::ChunkWriter::open(tmp.path() / "chunks");
    for (std::size_t i = 0; i < 7; ++i) {
        std::vector<std::uint8_t> data(20 + i, static_cast<std::uint8_t>(i));
        ASSERT_TRUE(w.write(c::Encoding::XOR, data).has_value());
    }
    ASSERT_TRUE(w.close().has_value());

    auto rdr = *b::ChunkReader::open(tmp.path() / "chunks");
    auto infos = rdr.iterate_segment(1);
    ASSERT_TRUE(infos.has_value());
    EXPECT_EQ(infos->size(), 7u);
    for (std::size_t i = 0; i < infos->size(); ++i) {
        EXPECT_EQ((*infos)[i].encoding, c::Encoding::XOR);
        EXPECT_EQ((*infos)[i].data_size, 20U + i);
    }
}

TEST(ChunkReader, ReadsUpstreamGoldenChunksFile) {
    auto rdr = *b::ChunkReader::open(golden_block_dir() / "chunks");
    EXPECT_EQ(rdr.segment_count(), 1u);
    auto seqs = rdr.segment_seqs();
    ASSERT_EQ(seqs.size(), 1u);
    EXPECT_EQ(seqs[0], 1u);
    auto infos = rdr.iterate_segment(seqs[0]);
    ASSERT_TRUE(infos.has_value());
    // Golden block has 102 chunks per its meta.json.
    EXPECT_EQ(infos->size(), 102u);
    // All chunks are XOR encoded.
    for (const auto& info : *infos) {
        EXPECT_EQ(info.encoding, c::Encoding::XOR);
    }
    // Every chunk reads + CRC-validates without error, and decodes into a
    // non-empty XOR chunk.
    for (const auto& info : *infos) {
        auto payload = rdr.read(info.ref);
        ASSERT_TRUE(payload.has_value());
        c::XORChunk ch = c::XORChunk::from_bytes(payload->data);
        EXPECT_GT(ch.num_samples(), 0);
        // Confirm we can iterate at least one sample without error.
        auto it = ch.iterator();
        EXPECT_TRUE(it.next());
        EXPECT_FALSE(it.error().has_value());
    }
}
