# merlion-tsdb-cpp

Modern C++23 reimplementation of the [Prometheus](https://github.com/prometheus/prometheus) TSDB storage engine.

> **Status: pre-alpha** — chunk encoder shipped, WAL in progress. Live progress in [ROADMAP.md](ROADMAP.md).

## Goals

- **Wire-format compatible** with Prometheus v3.x blocks, WAL segments, and chunk files — drop-in for `/data` directories produced by upstream Go Prometheus. Binary layouts are pinned in [SPEC.md](SPEC.md), the shared specification with [`merlion-tsdb-rs`](https://github.com/MerlionOS/merlion-tsdb-rs).
- **Modern C++23**: `std::expected`, `std::flat_map`, `std::print`, ranges, concepts. No legacy patterns.
- **Tested against Go-produced golden vectors** at every layer (chunks → WAL → block index).
- Eventually paired with a parallel Rust port ([`merlion-tsdb-rs`](https://github.com/MerlionOS/merlion-tsdb-rs)) for cross-implementation validation.

## Non-goals (for now)

- PromQL engine, scrape loop, service discovery, remote write/read, Web UI — these live in upstream Prometheus and may be ported later as separate projects.

## Scope (initial)

| Component | Upstream Go | Status |
|---|---|---|
| Chunk encoding (XOR/Gorilla, histograms) | `tsdb/chunkenc/` | planned |
| WAL reader/writer | `tsdb/wlog/` | planned |
| Head block (in-memory) | `tsdb/head.go` | planned |
| Persistent block + index v3 | `tsdb/block.go`, `tsdb/index/` | planned |
| Compactor | `tsdb/compact.go` | planned |
| Tombstones | `tsdb/tombstones/` | planned |

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
  chunkenc/   XOR / Gorilla / histogram encoders
  wal/        Write-ahead log
  head/       In-memory head block
  block/      Persistent block reader/writer
  encoding/   Varint, CRC32, bit-stream primitives
include/merlion_tsdb/   Public headers
tests/        GoogleTest unit tests
bench/        Google Benchmark microbenchmarks (planned)
testdata/     Golden fixtures (Go-produced for binary parity)
cmake/        Toolchain + helpers
```

## License

[Apache License 2.0](LICENSE) — same as upstream Prometheus. See [NOTICE](NOTICE) for attribution.
