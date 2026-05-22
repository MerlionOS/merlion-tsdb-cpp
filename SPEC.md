# Prometheus TSDB — On-Disk Format Specification

This document specifies the byte-level formats that `merlion-tsdb-cpp` and `merlion-tsdb-rs` implement. The objective is **wire-format compatibility** with upstream Prometheus v3.x — any directory written by upstream Go Prometheus must be readable by both impls, and vice versa.

**Reference points** (in priority order when this spec is ambiguous):
1. This document.
2. Upstream Go source at `../prometheus/tsdb/` (Prometheus v3.11.3).
3. The C++ reference implementation at `../merlion-tsdb-cpp/`.

Disagreement between any two of the three is a spec bug — fix the spec first, then the implementations.

**Specification status legend:**
- ✅ **Final** — implemented in at least one impl, tested, byte-exact.
- 🟡 **Drafted** — spec'd here but not yet implemented.
- ⬜ **TBD** — to be specified when the corresponding subsystem is implemented in C++.

---

## §1. Introduction

A Prometheus TSDB directory has the shape:

```
data/
├── 01HZ.../              # persistent block (immutable, ULID-named)
│   ├── chunks/
│   │   └── 000001        # chunk segment file
│   ├── index             # postings + symbol table + series
│   ├── meta.json         # block metadata
│   └── tombstones        # per-series deletion intervals
├── chunks_head/
│   └── 000000            # mmapped head chunks (current write-side)
└── wal/
    ├── 00000000          # WAL segments
    ├── 00000001
    └── checkpoint.000005/
```

This spec walks bottom-up: encoding primitives (§2) → chunk encoders (§3) → WAL (§4) → head (§5) → persistent block (§6) → tombstones (§7).

---

## §2. Wire format conventions ✅

### §2.1 Endianness ✅

All multi-byte integers are **big-endian** unless explicitly stated otherwise. The XOR chunk's 2-byte sample-count header, the WAL record length, the index TOC offsets, the chunk-segment magic, etc., are all big-endian.

The one notable exception: the chunk-disk-mapper's `ChunkDiskMapperRef` is a `(seq << 32) | offset` pair stored in **host byte order** within memory only — it never hits disk.

### §2.2 Varint (uvarint and zigzag varint) ✅

Both functions exactly match Go's `encoding/binary`:

#### Uvarint (LEB128)

```
encode(x: u64) -> [u8]:
    while x >= 0x80:
        emit (x & 0x7F) | 0x80    # 7 data bits + continuation bit set
        x >>= 7
    emit x                         # final byte: continuation bit clear
```

- **Maximum length**: 10 bytes for `u64` (9 full 7-bit groups + 1 byte for the high bit).
- **Overflow check on decode**: at byte index 9 (the 10th byte), the only valid values are `0x00` and `0x01`. Any larger value means the encoded value didn't fit in a `u64`.

#### Varint (signed)

Zigzag-then-uvarint:

```
encode(x: i64) -> [u8]:
    ux = (cast(x, u64) << 1) ^ cast(x >> 63, u64)   # arithmetic shift
    return uvarint_encode(ux)

decode(buf) -> i64:
    ux = uvarint_decode(buf)
    x = cast(ux >> 1, i64)
    if ux & 1: x = !x          # bitwise NOT to recover negatives
    return x
```

Zigzag mapping: `0 → 0, -1 → 1, 1 → 2, -2 → 3, 2 → 4, …`. Small-magnitude signed integers compress to small uvarints.

#### Test vectors

These bytes are produced by `Go encoding/binary.PutUvarint` / `PutVarint` and must round-trip exactly:

| Value (signed/unsigned) | Function | Bytes |
|---|---|---|
| `0u64` | uvarint | `00` |
| `1u64` | uvarint | `01` |
| `127u64` | uvarint | `7F` |
| `128u64` | uvarint | `80 01` |
| `300u64` | uvarint | `AC 02` |
| `16384u64` | uvarint | `80 80 01` |
| `u64::MAX` | uvarint | `FF FF FF FF FF FF FF FF FF 01` |
| `0i64` | varint | `00` |
| `-1i64` | varint | `01` |
| `1i64` | varint | `02` |
| `-2i64` | varint | `03` |
| `63i64` | varint | `7E` |
| `-64i64` | varint | `7F` |
| `64i64` | varint | `80 01` |

#### Errors

- `EndOfStream` — `buf.is_empty()`.
- `UnexpectedEnd` — last consumed byte had the continuation bit set.
- `VarintOverflow` — more than `MAX_VARINT_LEN64 = 10` bytes consumed, or the 10th byte was `> 1`.

Tests: `merlion-tsdb-cpp/tests/encoding/varint_test.cpp` enforces every row above.

### §2.3 Bit stream (`bstream`) ✅

A pair of types — `BitWriter` and `BitReader` — that write and read **MSB-first** bit sequences. The Go canon is `tsdb/chunkenc/bstream.go`; the C++ canon is `merlion-tsdb-cpp/src/encoding/bstream.cpp`.

#### Writer state

```
BitWriter {
    stream: Vec<u8>      # the byte buffer
    count: u8            # how many right-most bits are still WRITABLE in the last byte
}                        # count == 0 means "the last byte is full; the next write must
                         # push a new byte first"
```

Invariant: `count` is in `0..=8`. After every write, `count` shrinks toward 0 and then refills to 7 on the next byte push.

#### `write_bit(bit: bool)`

```
if count == 0:
    stream.push(0)
    count = 8
if bit:
    stream.last_mut() |= 1 << (count - 1)
count -= 1
```

Bits are packed MSB-first: the very first `write_bit(true)` produces `0x80`, the second sets bit 6 of the same byte, etc.

#### `write_byte(b: u8)` — spans byte boundaries

This is **not** "append a byte". It writes 8 bits at the current bit offset, potentially crossing the current byte boundary:

```
if count == 0:
    stream.push(b)
    return
# Fill the count high bits of the partial last byte, then start a new byte
# carrying the remaining (8 - count) low bits.
stream.last_mut() |= b >> (8 - count)
stream.push(b << count)
# count is UNCHANGED — we wrote exactly 8 bits.
```

#### `write_bits(u: u64, nbits: u32)`

Writes the `nbits` right-most bits of `u`, MSB-first:

```
if nbits == 0: return                # SHIFTING BY 64 IS UB. Guard explicitly.
u <<= 64 - nbits
while nbits >= 8:
    write_byte((u >> 56) as u8)
    u <<= 8
    nbits -= 8
while nbits > 0:
    write_bit((u >> 63) != 0)
    u <<= 1
    nbits -= 1
```

**Pitfall**: do not pre-shift `u` by `64 - nbits` when `nbits == 0`; that's a shift-by-64, undefined behavior. Same for the mirror case in the reader (see below).

#### Reader state

```
BitReader<'a> {
    stream: &'a [u8]
    stream_offset: usize    # next byte index to load into `buffer`
    buffer: u64             # up to 8 buffered bytes, packed MSB-first
    valid: u8               # how many right-most bits in `buffer` are VALID
    last: u8                # cached copy of stream.last() taken at construction
}
```

The `last` field is a **TOCTOU guard**: in upstream Go a concurrent appender may rewrite the very last byte of a chunk while a reader is consuming it. Taking a copy at construction freezes the tail. (Our impls inherit the behavior even though we don't have concurrent writers yet — it costs nothing and preserves the invariant.)

#### `load_next_buffer(nbits_min: u8) -> bool`

Returns false on EOF. Fast path when there are at least 8 bytes ahead of the tail; otherwise a slow path that fills the buffer with whatever remains.

**Fast path** (offset + 8 < stream.len):
```
b = stream[offset..offset+8] interpreted as big-endian u64
buffer = b
offset += 8
valid = 64
```

The "fast path stops 8 bytes before the end" is intentional: it ensures the slow path always handles the last byte, where the cached `last` value is consulted (see above).

**Slow path** (≤ 8 bytes remaining):
```
nbytes = min((nbits_min / 8) + 1, remaining)
buffer = 0
if offset + nbytes == stream.len:           # final byte is in this read
    buffer |= last as u64
    skip = 1
else:
    skip = 0
for i in 0..(nbytes - skip):
    buffer |= (stream[offset + i] as u64) << (8 * (nbytes - i - 1))
offset += nbytes
valid = nbytes * 8 as u8
```

The slow path packs `nbytes` bytes into the **high** end of `buffer` (positions `nbytes-1 .. 0`), leaving the low end zero. `valid = nbytes * 8` so the reader's masking machinery works uniformly.

#### `read_bits(nbits: u8) -> Result<u64, ReadError>`

```
if nbits == 0: return Ok(0)
if valid == 0:
    if !load_next_buffer(nbits): return Err(EndOfStream)

if nbits <= valid:                                 # fast: within current buffer
    mask = if nbits == 64 { u64::MAX } else { (1 << nbits) - 1 }   # AVOID 1 << 64 UB
    valid -= nbits
    return Ok((buffer >> valid) & mask)

# spans buffer boundary: take all current bits, refill, take remainder
low_mask = (1u64 << valid) - 1                     # valid < 64 here, safe
remaining = nbits - valid
v = (buffer & low_mask) << remaining
valid = 0
if !load_next_buffer(remaining): return Err(EndOfStream)
hi_mask = (1u64 << remaining) - 1                  # remaining < 64 here, safe
v |= (buffer >> (valid - remaining)) & hi_mask
valid -= remaining
Ok(v)
```

**Critical pitfall** (already burned once in C++): in the "fast" branch when `nbits == 64`, the naive `(1 << 64) - 1` is undefined behavior. On Apple Silicon clang it silently evaluates to `0`, making `read_bits(64)` always return 0. Always special-case 64. The "spans buffer" branch cannot hit this — `remaining < 64` is guaranteed because the fast branch already handled `nbits <= valid` cases.

#### `read_byte() -> Result<u8, ReadError>`

Equivalent to `read_bits(8).map(|v| v as u8)`. Used by `read_uvarint` / `read_varint`.

#### `read_uvarint` / `read_varint`

Identical algorithm to §2.2 but reads bytes via `read_byte` (which goes through the bit-buffer machinery). This matters because varints in XOR chunks are written via `write_byte` and may not be byte-aligned in the stream.

#### Test vectors

| Operation | Input | Expected output |
|---|---|---|
| `write_bit(true)` ×8 with pattern `10110010` | — | `[0xB2]` |
| `write_bit(true)` then `write_byte(0xAB)` | — | `[0xD5, 0x80]` |
| `write_bits(0xBEEF, 16)` | — | `[0xBE, 0xEF]` |
| Random roundtrip ×200 with `nbits ∈ [0, 64]` | — | matches |
| `write_bits(0xDEADBEEFCAFEBABE, 64)` ×20 | — | 160 bytes, read back 20×identical |

Tests: `merlion-tsdb-cpp/tests/encoding/bstream_test.cpp` (19 cases, also passes under ASan/UBSan).

---

## §3. Chunk encoders

### §3.1 XOR / Gorilla (`Encoding::Xor`, tag = 1) 🟡

Algorithm from the [Gorilla paper](https://www.vldb.org/pvldb/vol8/p1816-teller.pdf), adapted by Damian Gryski (`go-tsz`) and minor-tweaked by Prometheus. Encodes a sequence of `(timestamp: i64, value: f64)` samples into a self-contained byte slice.

#### Chunk header

The first **2 bytes** (`chunkHeaderSize`) are a big-endian `u16` holding the number of samples appended so far. An empty chunk's bytes are `[0x00, 0x00]`. After each append, rewrite these two bytes via `BitWriter::bytes_mut()`.

The bit stream of compressed sample data starts at byte offset 2.

#### Sample 0 (first append)

```
signed varint:   t                          # absolute timestamp
64 raw bits:     value's IEEE 754 bit pattern as written by f64::to_bits()
```

Both are emitted via `write_byte` (varint) and `write_bits(_, 64)` (raw f64 bits).

#### Sample 1 (second append)

```
unsigned varint: t_delta = t - t_prev       # MUST be >= 0
xor_write(value, value_prev, &mut leading, &mut trailing)
```

#### Sample N (N ≥ 2)

The timestamp encoding switches to **delta-of-delta** (dod) with a variable-length prefix code:

```
dod = (t - t_prev) - t_delta_prev      # i64

match dod:
    0:                       write bit `0`                             # 1 bit total
    fits in 14 bits signed:  write bits `10` (2 bits) + 14-bit dod     # 16 bits total
    fits in 17 bits signed:  write bits `110` (3 bits) + 17-bit dod    # 20 bits total
    fits in 20 bits signed:  write bits `1110` (4 bits) + 20-bit dod   # 24 bits total
    otherwise:               write bits `1111` (4 bits) + 64-bit dod   # 68 bits total

t_delta = t - t_prev
xor_write(value, value_prev, &mut leading, &mut trailing)
```

The signed-fit check is `bit_range(x, n) = -(2^(n-1) - 1) <= x <= 2^(n-1)`. **Note the asymmetric range** — this is intentional and matches Go.

When writing the 14-bit case, Go combines the prefix `0b10` with the top 6 bits of the dod into a single `write_byte(0b10_aaaaaa)` then a second `write_byte(bbbbbbbb)` for the low 8 bits. The wider cases use `write_bits` directly. (Either pattern produces the same bit sequence; choose the one your `BitWriter` makes idiomatic.)

#### `xor_write(new_v, prev_v, leading: &mut u8, trailing: &mut u8)`

The value compression layer. Encodes the XOR of the IEEE 754 bit patterns.

```
delta = new_v.to_bits() ^ prev_v.to_bits()

if delta == 0:
    write_bit(0)
    return

write_bit(1)

new_leading = delta.leading_zeros() as u8
new_trailing = delta.trailing_zeros() as u8

# Clamp to avoid overflowing the 5-bit field below.
if new_leading >= 32:
    new_leading = 31

# Can we reuse the previous (leading, trailing) window?
if *leading != 0xFF                 # 0xFF = sentinel "not yet initialized"
   and new_leading >= *leading
   and new_trailing >= *trailing:
    write_bit(0)
    write_bits(delta >> *trailing, 64 - *leading - *trailing)
    return

# Otherwise emit a fresh window.
*leading = new_leading
*trailing = new_trailing

write_bit(1)
write_bits(new_leading as u64, 5)

sigbits = 64 - new_leading - new_trailing
# If sigbits would be 64, encode as 0 (only 6 bits available). 0 cannot legitimately
# occur — sigbits == 0 means delta == 0, which the early-return above handled.
# The reader decodes 0 back as 64.
write_bits(sigbits as u64, 6)

write_bits(delta >> new_trailing, sigbits as u32)
```

**Initial state**: when constructing the appender for an empty chunk, set `leading = 0xFF` (sentinel). It will be overwritten on the first non-zero-delta sample.

#### Reading

The decoder is the strict inverse. The iterator reads the 2-byte header to learn `num_total`, then for each sample:

- **Sample 0**: `read_varint()` → `t`, `read_bits(64)` → `value.to_bits()`.
- **Sample 1**: `read_uvarint()` → `t_delta`, then `xor_read(...)`.
- **Sample N**: read 1–4 bits to identify the prefix:
  - `0` → dod = 0
  - `10` → read 14 bits, sign-extend if `bits > (1 << 13)` by subtracting `1 << 14`
  - `110` → 17 bits, sign-extend if `bits > (1 << 16)` by subtracting `1 << 17`
  - `1110` → 20 bits, sign-extend if `bits > (1 << 19)` by subtracting `1 << 20`
  - `1111` → `read_bits(64)`, cast to `i64`
- Then `xor_read(&mut value, &mut leading, &mut trailing)`:

```
if read_bit() == 0: value unchanged, return
if read_bit() == 0:
    # reuse leading/trailing
    mbits = 64 - leading - trailing
else:
    leading = read_bits(5) as u8
    mbits = read_bits(6) as u8
    if mbits == 0: mbits = 64              # see encoder note about sigbits==64
    trailing = 64 - leading - mbits
bits = read_bits(mbits)
value = f64::from_bits(value.to_bits() ^ (bits << trailing))
```

#### Invariants

- A chunk that has been compacted is still readable; only the underlying `Vec` capacity changes (`compact()` is a memory-recovery optimization).
- `num_total` (the header u16) must be incremented **after** the bit-stream writes succeed.
- Calling `appender()` on a non-empty chunk requires replaying existing samples to recover the iterator state (`t`, `t_delta`, `value`, `leading`, `trailing`) — there is no way to short-circuit this.
- Multiple `Iterator`s on the same chunk are safe to use concurrently with **at most one** active `Appender`, provided the appender writes only via `BitWriter::write_*` and the `TOCTOU` cached-`last` mechanism is in place. Two appenders concurrently is a logic error.

#### Bounds

- `MAX_BYTES_PER_XOR_CHUNK = 1024`
- Hard cap on samples: `u16::MAX = 65_535` (the header is a 2-byte counter). Appending the 65,536th sample is a panic / `ChunkFull` error.

#### Reference implementations

- Go: `prometheus/tsdb/chunkenc/xor.go`
- C++: `merlion-tsdb-cpp/src/chunkenc/xor.cpp` (in progress at the time of writing this section).

### §3.2 XOR2 (`Encoding::Xor2`, tag = 4) ⬜

Newer encoding adding stale-NaN markers and start-timestamp (ST) support. The control-prefix code has six cases instead of five. To be specified when implemented in C++. Reference: `prometheus/tsdb/chunkenc/xor2.go` and `bstreamReader::readXOR2Control`.

### §3.3 Histogram (`Encoding::Histogram`, tag = 2) ⬜

Sparse histogram chunk. Reference: `prometheus/tsdb/chunkenc/histogram.go`.

### §3.4 Float histogram (`Encoding::FloatHistogram`, tag = 3) ⬜

Reference: `prometheus/tsdb/chunkenc/float_histogram.go`.

---

## §4. Write-ahead log (WAL) ⬜

To be specified when implemented in C++. Salient facts gathered from the Go reference:

- Magic: `0x7AD7A3`, format version 1. Files live in `wal/000000`, `wal/000001`, etc.
- Segments default to 128 MiB.
- Records are written into 32 KiB **pages**. Each record has a 7-byte header: `type (1B) | length (4B BE) | CRC32 (4B BE)`. CRC is over the record body.
- Records span pages: a record's body is split across page boundaries, each fragment carrying its own header with a fragment-marker bit in the type byte.
- Record types are enumerated in `record/record.go`: `Series`, `Samples`, `SamplesV2`, `Tombstones`, `Exemplars`, `Histograms`, `FloatHistograms`, `Metadata`, `MmapMarkers`.

Reference: `prometheus/tsdb/wlog/wlog.go`.

---

## §5. Head block (in-memory) ⬜

To be specified. Not strictly an on-disk format, but defines the bridge between WAL replay and persistent-block compaction. Reference: `prometheus/tsdb/head.go`.

---

## §6. Persistent block ⬜

### §6.1 `meta.json` ⬜

JSON document with fields `version`, `ulid`, `minTime`, `maxTime`, `stats {numSamples, numSeries, numChunks}`, `compaction {level, sources, parents, deletable, hints}`. Version 1 is the only currently-shipped format. Reference: `prometheus/tsdb/block.go:164`.

### §6.2 Index v3 ⬜

Magic `0xBAAAD700`. Sections: symbol table, series, label indices, postings, postings offset table, TOC. TOC is the last 56 bytes (7 × `u64` BE offsets + a `u32` CRC). Reference: `prometheus/tsdb/index/index.go`.

### §6.3 Chunks segment ⬜

Magic `0x85BD40DD`. Header = `magic (4) | version (1) | padding (3)`. Per chunk: `length: uvarint | encoding: u8 | data | crc32: u32 BE`. Reference: `prometheus/tsdb/chunks/chunks.go`.

### §6.4 Head chunks file (`chunks_head/`) ⬜

Distinct from §6.3. Magic `0x0130BC91`. The `ChunkDiskMapperRef` encodes `(seq << 32) | offset` and includes a series-ref prefix per chunk. Reference: `prometheus/tsdb/chunks/head_chunks.go`.

---

## §7. Tombstones ⬜

Per-series deletion intervals. File format defined by `prometheus/tsdb/tombstones/tombstones.go`. Magic `0x0130BC91` (shares the head-chunks magic — distinguish by file location). Reference: same package.

---

## Appendix A. Pitfalls catalogue

Real bugs encountered or anticipated during porting. Each line is a "do not repeat":

1. **`1 << 64` is UB.** In `read_bits(64)` the mask computation must special-case 64, or the read silently returns 0 on Apple Silicon clang (see §2.3). Same trap in any code that synthesizes `(1 << n) - 1` masks dynamically.
2. **`x << (64 - nbits)` is UB when `nbits == 0`**. Guard `write_bits(_, 0)` early.
3. **`write_byte` is NOT byte-aligned append.** It writes 8 bits at the current bit offset. Confusing it with `Vec::push` will corrupt every subsequent bit position.
4. **`leading = 0xFF` is the sentinel for "uninitialized"**, not a real leading-zero count. Treat any code path that checks `leading != 0xFF` carefully — it must remain consistent between encoder and decoder.
5. **`sigbits = 0` is encoded as the 6-bit value `0` but decoded as 64**. There's no legitimate `sigbits == 0` case because `delta == 0` is short-circuited earlier.
6. **`bit_range(x, n)` is asymmetric**: `-(2^(n-1) - 1) <= x <= 2^(n-1)`. Note the `-1` on the lower bound only. Off-by-one here will silently push values into the wrong prefix bucket.
7. **The 14-bit dod case** is written as two `write_byte` calls in Go (`0b10aaaaaa` then `bbbbbbbb`), not via `write_bits`. The bits on the wire are the same; the call shape can differ.
8. **Header rewrite after each append.** The 2-byte sample-count header is mutated in place via `bytes_mut()`. If your `BitWriter` returns a copy, you'll silently lose the increment.
9. **Big-endian everywhere except `ChunkDiskMapperRef`**. Don't reach for `u64::from_le_bytes` reflexively.

---

## Appendix B. Glossary

- **bstream** — bit stream, MSB-first.
- **dod** — delta of delta (`Δ(Δt)`).
- **leading / trailing** — number of zero bits at the top / bottom of a 64-bit XOR delta.
- **mbits / sigbits** — significant bits = `64 - leading - trailing`.
- **ULID** — 26-character lexicographic UUID; used as block directory name.
- **TOCTOU** — time-of-check-to-time-of-use; here, racing the chunk's last byte between concurrent reader and writer.

---

## Changelog

- **2026-05-22** — Initial draft. §1, §2, §3.1 in detail; §3.2–§7 are skeletons pending implementation.
