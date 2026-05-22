# Roadmap

Single-pane status for the Merlion TSDB rewrite. Updated on every merged PR.

> **Goal**: Wire-format–compatible reimplementation of the Prometheus v3.x TSDB storage engine. Two parallel implementations (`merlion-tsdb-cpp` + [`merlion-tsdb-rs`](https://github.com/MerlionOS/merlion-tsdb-rs)) cross-validated against each other and against upstream Go.

## Subsystem status

Legend: ✅ landed · 🟡 in progress · ⬜ not started · ⬛ out of scope

| # | Subsystem | C++ | Rust | SPEC.md | Notes |
|---|---|---|---|---|---|
| 1 | `encoding/varint` (LEB128 + zigzag) | ✅ | ✅ | §2.2 ✅ | Byte-for-byte parity with Go `encoding/binary` |
| 2 | `encoding/bstream` (bit I/O, MSB-first) | ✅ | ✅ | §2.3 ✅ | `1<<64` UB pitfall catalogued |
| 3 | `encoding/crc32c` (Castagnoli) | ✅ | ✅ | §2.4 ✅ | NOT IEEE — `0x1EDC6F41` polynomial |
| 4 | `chunkenc/xor` (Gorilla) | ✅ | ✅ | §3.1 ✅ | All 5 dod prefix buckets exercised |
| 5 | `wal` segments + records | ✅ | ⬜ | §4 ✅ | 32 KiB pages, 7-byte header, torn-tail tolerant |
| 6 | `head` (in-memory + WAL replay) | ✅ | ⬜ | §5 ✅ | MemSeries, SeriesStore, WAL append/replay |
| 7 | `block/meta` (meta.json) | ✅ | ⬜ | §6.1 ✅ | ULID, compaction.level, sources |
| 8 | `block/chunks` segment files | ✅ | ⬜ | §6.3 ✅ | 0-indexed `BlockChunkRef.seq` (not filename int) |
| 9 | `block/index` reader (V1/V2/V3) | ✅ | ⬜ | §6.2 ✅ | symbols + series + postings + offset table |
| 10 | `block/index_writer` (emits V2) | ✅ | ⬜ | §6.6 ✅ | 16-byte series alignment, sparse symbol table |
| 11 | `block/ulid` (Crockford b32) | ✅ | ⬜ | §6.5 ✅ | First char `'0'..'7'`; ms timestamp + 80 random bits |
| 12 | `block` writer (create_from_series, create_from_head) | ✅ | ⬜ | §6.6, §6.7 ✅ | Atomic head→block flush |
| 13 | `compactor` (vertical-merge dedup) | ✅ | ⬜ | §7 ✅ | Decode → sort → last-input-wins → re-encode |
| 14 | `model/matcher` (Eq/Neq/Re/Nre) | ✅ | ⬜ | §8.2 ✅ | Regex anchored `^(...)$` |
| 15 | `block::select` (matchers + time range) | ✅ | ⬜ | §8 ✅ | Posting-list intersect, empty-value PromQL semantics |
| 16 | `querier::Querier` (cross-block) | ✅ | ⬜ | §9 ✅ | Hash-bucket merge, chunks sorted by min_time |
| 17 | `head::select` (in-memory querier) | ⬜ | ⬜ | §10 ⬜ | Mix Head + Block in one query |
| 18 | `chunkenc/histogram` (integer + float) | ⬜ | ⬜ | §3.3-3.4 ⬜ | Reuses xor_write primitive |
| 19 | `tombstones` (per-series deletion intervals) | 🟡 | ⬜ | §6.8 🟡 | MVP writes empty file; read deferred |
| 20 | Cross-impl byte-level validation harness | ⬜ | ⬜ | — | Go fixturegen tool + diff |
| — | PromQL / scrape / discovery / UI | ⬛ | ⬛ | — | Separate projects, not this repo |

**Current totals (C++)**: 231 GoogleTest cases passing on `main` under Debug + ASan/UBSan. SPEC.md §1-§9 all Final.

## Next milestones

### C++ side
1. **§10 `head::select`** — Head implements the same matcher+time-range surface as `Block::select`, so a unified Querier can mix in-memory and on-disk data within a single query.
2. **`chunkenc/histogram`** — integer and float histogram encoders (§3.3, §3.4). Reuses `xor_write` primitive.
3. **Tombstone read path** — currently MVP writes an empty tombstones file (§6.8). Reading + applying deletion intervals at query time is deferred.
4. **Cross-impl validation harness** — small Go fixturegen tool emitting deterministic bytes per encoder, so the matrix below becomes mechanical.

### Rust side (Codex-driven from shared SPEC.md)
1. **§4 WAL** — segment writer/reader, 7-byte header with u16 BE length, CRC32C, torn-tail recovery. Spec already pinned.
2. **§5 Head** — MemSeries + SeriesStore + WAL append/replay loop.
3. **§6 Persistent block** — meta + chunks + index reader, then index writer, then `Block::create_from_series` / `create_from_head`.

## Cross-impl validation status

| Producer | Consumer | Layer covered | Status |
|---|---|---|---|
| Go (upstream) | C++ | Reading `testdata/index_format_v1/` block | ✅ end-to-end roundtrip passes |
| Go (upstream) | Rust | Same | ⬜ once Rust block reader lands |
| C++ | Rust | XOR chunks, WAL, blocks | ⬜ pending fixturegen tool |
| Rust | C++ | Same | ⬜ |
| C++ | C++ | Internal write→read roundtrip | ✅ `block_writer_test.cpp::CrossValidationAggregateQueryRecoversAllSeries` |
| Rust | Rust | Internal write→read roundtrip | 🟡 xor only |

A small Go program at `tools/fixturegen/` (planned, not yet started) will emit deterministic byte fixtures for each encoder so cross-impl checks become diff-based.

## Conventions

- **Worktree + PR workflow** is mandatory. Multiple Claude Code and Codex CLI sessions work on this repo concurrently; direct pushes to `main` collide. Every change lands via `gh pr create`.
- **SPEC.md is the contract.** A section moves to ✅ Final only after at least one impl ships passing tests against the spec. New subsystems get a Drafted (🟡) section first, then promoted.
- **Apache-2.0**, matching upstream Prometheus. NOTICE preserves upstream attribution; no Go code is copied.

## Recent activity

| Date | PR | What |
|---|---|---|
| 2026-05-23 | [#25](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/25) | §9 cross-block Querier — hash-bucket merge, chunks sorted across blocks |
| 2026-05-23 | [#24](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/24) | §8 Querier — Matcher (Eq/Neq/Re/Nre) + `Block::select` |
| 2026-05-23 | [#23](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/23) | §7 vertical-merge dedup in compactor (last-input-wins) |
| 2026-05-22 | [#22](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/22) | SPEC.md §4-§7 promoted to Final; pitfalls catalogue → 19 entries |
| 2026-05-22 | [#21](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/21) | `Block::compact` — multi-block merge with level promotion |
| 2026-05-22 | [#20](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/20) | `Block::create_from_head` — head → block flush |
| 2026-05-22 | [#19](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/19) | ULID generator + `Block::create_from_series` |
| 2026-05-22 | [#18](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/18) | `IndexWriter` — V2/V3 index file emission |
| 2026-05-22 | [#17](https://github.com/MerlionOS/merlion-tsdb-cpp/pull/17) | Phase 4 block reader (`Block::open`, `Block::query`) |
| 2026-05-21 | earlier | Head + WAL + chunks + index reader (Phases 1-3) |
