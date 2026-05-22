# testdata/

Golden fixtures used by C++ unit tests for byte-level format parity with upstream Go Prometheus.

## Provenance

Files under `index_format_v1/` and `repair_index_version/` are copied verbatim from upstream Prometheus:

- Source: <https://github.com/prometheus/prometheus> v3.11.3, path `tsdb/testdata/`
- License: Apache-2.0 (same as this project — see top-level `LICENSE` and `NOTICE`)

Do **not** modify these files. They are canonical wire-format references.

## Contents

### `index_format_v1/`

A complete v1-format block (102 series, 102 samples, 0–7,200,000ms range). Use this to validate:

- `chunks/000001` — chunk segment reader (magic `0x85BD40DD`)
- `index` — v1 index reader (must be readable by v3 reader for backward compat)
- `meta.json` — block metadata parser
- `tombstones` — tombstone reader (empty in this fixture)

### `repair_index_version/01BZJ9WJQPWHGNC2W4J9TA62KC/`

An index file with an older format version, used in upstream's index-repair tests. Useful for testing version detection and migration paths.

## Missing fixtures (to generate)

Upstream does not ship golden byte vectors for individual encoders — its tests synthesize inputs at runtime. We will add a small Go program under `tools/fixturegen/` (planned) that produces deterministic byte fixtures for:

- Single XOR chunk with known float samples
- Single histogram chunk
- WAL segment containing series + samples + tombstone records
- Head chunks file with known mmap layout

Cross-validation strategy: C++ encoder writes → Go decoder reads same bytes, and vice versa. Once `merlion-tsdb-rs` exists, both impls validate against the same fixtures and against each other.
