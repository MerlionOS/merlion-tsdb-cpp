# Roadmap

Single-pane status for the Merlion TSDB rewrite. Updated on every merged PR.

> **Goal**: Wire-format–compatible reimplementation of the Prometheus v3.x TSDB storage engine. Two parallel implementations (`merlion-tsdb-cpp` + [`merlion-tsdb-rs`](https://github.com/MerlionOS/merlion-tsdb-rs)) cross-validated against each other and against upstream Go.

## Subsystem status

Legend: ✅ landed · 🟡 in progress · ⬜ not started · ⬛ out of scope

| # | Subsystem | C++ | Rust | SPEC.md | Tests | Notes |
|---|---|---|---|---|---|---|
| 1 | `encoding/varint` (LEB128 + zigzag) | ✅ | ✅ | §2.2 ✅ | 7 | Cross-checked against Go byte tables |
| 2 | `encoding/bstream` (bit I/O) | ✅ | ✅ | §2.3 ✅ | 12 | UB pitfall (`1<<64`) catalogued |
| 3 | `chunkenc/xor` (Gorilla) | ✅ | 🟡 | §3.1 ✅ | 12 | All 5 dod prefix buckets exercised |
| 4 | `wal` segments + records | 🟡 | ⬜ | §4 ⬜ | — | In progress, branch `claude/wal` |
| 5 | `chunks` segment file | ⬜ | ⬜ | §6.3 ⬜ | — | Persistent block sub-layer |
| 6 | `chunks_head` (mmapped) | ⬜ | ⬜ | §6.4 ⬜ | — | Bridge between head and block |
| 7 | `head` (in-memory) | ⬜ | ⬜ | §5 ⬜ | — | Depends on chunkenc + WAL |
| 8 | `index` (postings, symbols, TOC) | ⬜ | ⬜ | §6.2 ⬜ | — | v3 format |
| 9 | `tombstones` | ⬜ | ⬜ | §7 ⬜ | — | Per-series deletion intervals |
| 10 | `block` (meta.json + readers) | ⬜ | ⬜ | §6 ⬜ | — | Top-level persistent block |
| 11 | `compactor` | ⬜ | ⬜ | — | — | Level merge, dedup |
| 12 | `chunkenc/xor2` | ⬜ | ⬜ | §3.2 ⬜ | — | After xor lands cleanly |
| 13 | `chunkenc/histogram` (integer + float) | ⬜ | ⬜ | §3.3-3.4 ⬜ | — | Reuses xor_write primitive |
| 14 | Cross-impl byte-level validation | ⬜ | ⬜ | — | — | Go fixturegen tool + diff harness |
| — | PromQL / scrape / discovery / UI | ⬛ | ⬛ | — | — | Separate projects, not this repo |

**Current totals**: 31 GoogleTest cases passing on `main` under Debug + ASan/UBSan.

## Next milestone: WAL MVP

To call WAL "MVP done":
1. Segment file open/create (`wal/000000`, `wal/000001`, …) with 128 MiB rollover ✅ TBD
2. 32 KiB page-buffered writer
3. Record header (`type | length | CRC32`) + record-spans-page handling
4. CRC32 (IEEE polynomial) helper
5. Series and SamplesV2 record encoders
6. Replay loop that handles partial last record gracefully
7. Round-trip property test: write N records, replay, recover identical sequence
8. Read at least one upstream Go-produced WAL segment (need to extract one to `testdata/` first)

## Cross-impl validation status

Once both C++ and Rust have a subsystem landed, the validation matrix is:

| Producer | Consumer | Layer covered |
|---|---|---|
| Go | C++ | Reading existing `testdata/index_format_v1/chunks/` |
| Go | Rust | Same — once Rust block reader lands |
| C++ | Rust | XOR chunks (pending — needs fixturegen tool) |
| Rust | C++ | Same |
| C++ | C++ | Internal round-trip (currently the only validation) |
| Rust | Rust | Internal round-trip |

A small Go program at `tools/fixturegen/` (planned, not yet started) will emit deterministic byte fixtures for each encoder so the matrix becomes mechanical.

## Conventions

- **Worktree + PR workflow** is mandatory. Multiple Claude Code sessions and Codex CLI sessions work on this repo concurrently; direct pushes to `main` collide. Every change lands via `gh pr create`. See [`feedback_worktree_pr_workflow.md`](https://github.com/MerlionOS/merlion-tsdb-cpp) (memory) for rationale.
- **SPEC.md is the contract.** A section moves to ✅ Final only after at least one impl ships passing tests against the spec. New subsystems get a Drafted (🟡) section first, then promoted.
- **Apache-2.0**, matching upstream Prometheus. NOTICE preserves upstream attribution; no Go code is copied.

## Recent activity

| Date | PR / commit | What |
|---|---|---|
| 2026-05-22 | `780369f` | XOR/Gorilla chunk encoder + 12 tests |
| 2026-05-22 | `fe92406` | SPEC.md v1 (§1, §2, §3.1 final) |
| 2026-05-22 | `42145b1` | bstream + varint primitives + 19 tests |
| 2026-05-22 | `95d1ad9` | Import golden testdata from upstream |
| 2026-05-22 | `fff81e6` | CMake + LLVM 22 + C++23 bootstrap |
