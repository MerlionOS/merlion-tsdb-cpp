# merlion-tsdb-cpp

Modern C++23 reimplementation of the [Prometheus](https://github.com/prometheus/prometheus) TSDB storage engine.

> **Status: alpha.** Read + write + compact + query paths (across persistent blocks AND the in-memory head, unified in one querier) all land bit-for-bit compatible with upstream v3.x format. 254 tests pass under Debug + ASan/UBSan. SPEC.md §1-§10 Final. Live progress in [ROADMAP.md](ROADMAP.md).

## Goals

- **Wire-format compatible** with Prometheus v3.x blocks, WAL segments, and chunk files — drop-in for `/data` directories produced by upstream Go Prometheus. Binary layouts are pinned in [SPEC.md](SPEC.md), the shared specification with [`merlion-tsdb-rs`](https://github.com/MerlionOS/merlion-tsdb-rs).
- **Modern C++23**: `std::expected`, `std::span`, `std::filesystem`, `std::bit_cast`, ranges. No legacy patterns.
- **Tested against Go-produced golden vectors** at every layer — the upstream `index_format_v1/` fixture (102 series) is read end-to-end on every CI run.
- Paired with a parallel Rust port ([`merlion-tsdb-rs`](https://github.com/MerlionOS/merlion-tsdb-rs)) for cross-implementation validation. Both impls work from the same SPEC.md.

## Non-goals (for now)

- PromQL engine, scrape loop, service discovery, remote write/read, Web UI — these live in upstream Prometheus and may be ported later as separate projects.

## Status

| Component | Upstream Go | Status |
|---|---|---|
| Encoding primitives (varint, bstream, CRC32C) | `tsdb/encoding/`, `model/textparse/` | ✅ Final |
| XOR/Gorilla chunk encoder | `tsdb/chunkenc/xor.go` | ✅ Final |
| WAL reader/writer | `tsdb/wlog/` | ✅ Final |
| Head block (in-memory + WAL replay) | `tsdb/head.go` | ✅ Final |
| Persistent block reader | `tsdb/block.go`, `tsdb/index/` | ✅ Final (V1/V2/V3 read) |
| Persistent block writer + ULID | `tsdb/block.go`, `oklog/ulid` | ✅ Final (V2 write) |
| Compactor (vertical-merge dedup) | `tsdb/compact.go` | ✅ Final |
| Matchers + block querier | `pkg/labels`, `tsdb.BlockQuerier` | ✅ Final |
| Cross-block querier | `storage.MergeSeriesSet` | ✅ Final |
| Head querier + unified Querier (Head + Block) | `tsdb.HeadQuerier`, `storage.MergeSeriesSet` | ✅ Final |
| Histogram chunks (integer + float) | `tsdb/chunkenc/histogram*.go` | ⬜ Next |
| Tombstones (read path) | `tsdb/tombstones/` | 🟡 (MVP writes empty) |

End-to-end proven: write 20 series → flush head → block → index → query through `Block::select` → decode → recover bit-identical samples. See `tests/block/block_writer_test.cpp::CrossValidationAggregateQueryRecoversAllSeries`.

## Build

Requires **Homebrew LLVM 22+** (not Apple Clang — libc++ in Apple's clang lacks C++23 library features) and CMake 3.28+.

```bash
brew install llvm cmake ninja

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release`, `relwithdebinfo`, `asan`, `tsan`, `bench`. The toolchain file (`cmake/toolchain-llvm.cmake`) pins the compiler to `/opt/homebrew/opt/llvm/bin/clang++` and links against Homebrew's libc++.

CLion: open the project directory; it auto-detects `CMakePresets.json`.

## Layout

```
src/
  encoding/     varint, CRC32C, bit-stream primitives
  chunkenc/     XOR / Gorilla encoder
  wal/          Write-ahead log (segments, records, pages)
  model/        Labels, Matcher
  head/         In-memory head block + WAL replay
  block/        Persistent block reader/writer (meta, chunks, index, ulid)
  querier/      Cross-block querier
include/merlion_tsdb/   Public headers (mirror of src/)
tests/        GoogleTest unit tests (~254 cases)
testdata/     Golden fixtures (Go-produced for binary parity)
cmake/        Toolchain + helpers
SPEC.md       The on-disk format spec (§1-§10 Final)
ROADMAP.md    Subsystem-by-subsystem status
```

## Using as a library

The current surface is C++ headers under `merlion_tsdb::`. The key entry points:

```cpp
// Open and query an existing block.
auto blk = merlion_tsdb::block::Block::open("data/01HX.../");
std::array ms{
    merlion_tsdb::model::Matcher::equal("__name__", "up"),
    merlion_tsdb::model::Matcher::equal("job", "api"),
};
auto results = blk->select(ms, /*mint=*/0, /*maxt=*/INT64_MAX);

// Or span multiple blocks AND the in-memory head in one query.
std::vector<const merlion_tsdb::block::Block*> blocks = /* ... */;
std::vector<const merlion_tsdb::head::Head*>   heads  = /* ... */;
merlion_tsdb::querier::Querier q{blocks, heads};
auto merged = q.select(ms, mint, maxt);
```

`MergedSeries::chunks[i].iterator()` walks samples for each XOR chunk in `min_time` order, with chunks from all sources (blocks + head) globally sorted by start time. Sample-level overlap pruning is the caller's job (consistent with upstream's chunk-handoff contract — see SPEC §8.1, §10.6).

## License

[Apache License 2.0](LICENSE) — same as upstream Prometheus. See [NOTICE](NOTICE) for attribution.
