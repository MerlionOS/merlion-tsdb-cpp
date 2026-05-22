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

### §3.1 XOR / Gorilla (`Encoding::Xor`, tag = 1) ✅

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
- C++: `merlion-tsdb-cpp/src/chunkenc/xor.cpp` — implemented; 12 test cases pass under Debug + ASan/UBSan, covering: empty chunk, single sample, two samples, constant value (xor=0 path), regular intervals (dod=0 path), irregular dods (all 5 prefix buckets), negative dods (sign-extension), 500-sample random fuzz with NaN/Inf, mid-chunk appender replay, `from_bytes` roundtrip, `compact()` idempotence.

### §3.2 XOR2 (`Encoding::Xor2`, tag = 4) ⬜

Newer encoding adding stale-NaN markers and start-timestamp (ST) support. The control-prefix code has six cases instead of five. To be specified when implemented in C++. Reference: `prometheus/tsdb/chunkenc/xor2.go` and `bstreamReader::readXOR2Control`.

### §3.3 Histogram (`Encoding::Histogram`, tag = 2) ⬜

Sparse histogram chunk. Reference: `prometheus/tsdb/chunkenc/histogram.go`.

### §3.4 Float histogram (`Encoding::FloatHistogram`, tag = 3) ⬜

Reference: `prometheus/tsdb/chunkenc/float_histogram.go`.

---

## §4. Write-ahead log (WAL) ✅

### §4.1 Filesystem layout

Each TSDB head owns a `wal/` directory containing one or more segment files:

```
wal/
├── 00000000
├── 00000001
├── 00000002
└── ...
```

- Segment names are **8-digit zero-padded decimal**, starting at `00000000`. (Note: the persistent block's `chunks/` directory uses a different naming scheme — 6-digit starting at `000001`. See §6.3.)
- Each segment is at most **128 MiB** by default. When a new record would push the current segment past the cap, the writer closes it (zero-pads to the next page boundary, fsyncs, advances the index) and opens the next.
- There is **no magic or version header at the start of a segment file**. Segments are sequences of 32 KiB pages.

### §4.2 Page format

- Page size is exactly **32 KiB (32 768 bytes)**.
- A page contains zero or more record fragments. Fragments do not span page boundaries; if a record's body is too long for the remaining room, it's split into multiple fragments across consecutive pages.
- When the room left in the current page is less than the 7-byte record header size, the writer **zero-pads** the page out to the next 32 KiB boundary. A reader encountering a leading zero byte where a record header is expected interprets it as a **PageTerm** sentinel (recType = 0) and skips to the next page.

### §4.3 Record framing

Every fragment is preceded by a 7-byte header, followed immediately by the fragment body. There is no trailing CRC after the body — the CRC is part of the header.

```
byte 0:        type        (1 byte)
bytes 1..2:    length      (u16 big-endian)   — size of the fragment body in bytes
bytes 3..6:    crc32c      (u32 big-endian)   — CRC32-Castagnoli over the fragment body only
                                                (NOT including the 7-byte header)
bytes 7..7+L:  body        (L bytes)
```

> **Correction vs. earlier drafts:** length is a **2-byte** u16, NOT a 4-byte u32. Maximum fragment body size is therefore `pageSize − headerSize = 32 768 − 7 = 32 761` bytes, which fits in u16 with room to spare.

### §4.4 Type byte

The 1-byte type field carries fragment metadata plus compression flags:

```
bit 7 6 5 4 3 2 1 0
        |--|---|---|
        |   |   bottom 3 bits = recType (0..4)
        |   bit 3 = Snappy compression
        bit 4 = Zstd compression
```

`recType` values:

| Value | Name      | Meaning                                                        |
|-------|-----------|----------------------------------------------------------------|
| 0     | PageTerm  | sentinel; rest of page is zero padding, advance to next page   |
| 1     | Full      | whole record fits in this one fragment                         |
| 2     | First     | first fragment of a multi-fragment record                      |
| 3     | Middle    | interior fragment (only present for records > 2 pages)         |
| 4     | Last      | final fragment                                                 |

**MVP readers reject** non-zero compression bits with a structured error rather than silently mis-decoding. Compression support is post-MVP; emit only uncompressed records.

### §4.5 Fragmenting a record across pages

To emit a record body of length L:

1. If the current page has fewer than 7 bytes left, zero-pad it and advance to the next page.
2. Compute the room available in the current page (`page_size - (offset_within_page) - 7`).
3. If L fits in that room → emit one **Full** fragment.
4. Otherwise → emit **First** with the first room-sized chunk, advance to next page, emit zero or more **Middle**s, then a final **Last** with the remainder.

Single-record empty bodies (L = 0) are legal and emit a Full fragment with length 0.

### §4.6 Torn-tail tolerance

A crashed writer can leave the last segment with an incomplete fragment sequence (e.g., a `First` with no matching `Last`). The reader's policy:

- A torn record at the tail of **the last segment** is silently dropped — the reader returns clean EOF after consuming every complete record before it.
- A torn record anywhere else (CRC mismatch, missing `Last`, etc.) is **fatal** and surfaces as a structured corruption error.

### §4.7 Records

The fragment body is the **record body**. Its first byte is the record type (a separate enumeration from `recType` above — don't confuse the two), and the rest is the type-specific payload.

Record type values (matching upstream's iota at `tsdb/record/record.go:35`):

| Value | Name                                | MVP?     |
|-------|-------------------------------------|----------|
| 1     | Series                              | ✅       |
| 2     | Samples (V1)                        | deferred |
| 3     | Tombstones                          | deferred |
| 4     | Exemplars                           | deferred |
| 5     | MmapMarkers                         | deferred |
| 6     | Metadata                            | deferred |
| 7..10 | Histogram variants                  | deferred |
| 11    | SamplesV2                           | ✅       |
| 12,13 | HistogramSamplesV2 / FloatHistogramSamplesV2 | deferred |
| 255   | Unknown                             | sentinel |

Readers silently skip unknown record types for forward compatibility against future writers.

#### §4.7.1 Series record (type = 1)

```
byte 0:                   type byte = 1
for each entry, in writer's emission order:
  bytes 0..7:             ref           — u64 big-endian SeriesRef (head-side ID)
  uvarint:                label_count   — number of labels
  for each label:
    uvarint-prefixed:     name          — UTF-8 bytes, length is the uvarint
    uvarint-prefixed:     value         — UTF-8 bytes, length is the uvarint
```

Empty Series records (just the type byte, no entries) are legal and represent "no new series in this batch".

#### §4.7.2 SamplesV2 record (type = 11)

The "V2" name comes from upstream's evolution: V1 (`type = 2`) lacks start-timestamp (ST) support; V2 adds it via a marker-byte scheme so the common case (ST == 0 or ST unchanged) stays compact.

```
byte 0:                   type byte = 11
if there is at least one sample, the first sample is encoded absolutely:
  varint:                 ref           — signed zigzag, absolute series ref
  varint:                 t             — signed zigzag, absolute timestamp
  varint:                 st            — signed zigzag, absolute start timestamp (0 = absent)
  bytes 0..7:             value         — u64 big-endian IEEE 754 bit pattern
for each subsequent sample:
  varint:                 ref_delta     — signed zigzag, delta from previous sample's ref
  varint:                 t_delta       — signed zigzag, delta from the FIRST sample's t
                                          (NOT the previous sample's t)
  byte 0:                 st_marker     ∈ {0, 1, 2}
    0 (noST):             — ST stays 0
    1 (sameST):           — ST equals the previous sample's ST
    2 (explicitST):
      varint:             st_delta_first — signed zigzag, delta from first.st
  bytes 0..7:             value         — u64 BE IEEE 754 bit pattern
```

Subtleties (every one of these was a real porting hazard):

- `ref_delta` is signed and may be **negative** (series refs aren't monotonic across appends).
- `t_delta` is the delta to **first.t**, not previous.t. Upstream made this choice for ST encoding symmetry; trying to use previous.t will compile and silently corrupt timestamps on multi-sample records.
- The ST marker byte must be in `{0, 1, 2}` exactly — any other value is corruption.
- `value` is the **IEEE 754 bit pattern** (`std::bit_cast<u64>(f64)` in C++ or `f64::to_bits()` in Rust) as a big-endian 64-bit integer. NOT a floating-point representation of any numeric form.

### §4.8 C++ reference

- `merlion-tsdb-cpp/src/wal/page.cpp` — page + fragment framing
- `merlion-tsdb-cpp/src/wal/segment_writer.cpp` — file I/O, 128 MiB rollover, fsync
- `merlion-tsdb-cpp/src/wal/segment_reader.cpp` — multi-segment iteration, torn-tail tolerance
- `merlion-tsdb-cpp/src/wal/record.cpp` — Series + SamplesV2 encoders / decoders

Tests under `tests/wal/` cover every framing, type-byte, and torn-tail edge case enumerated above; if your implementation passes the same scenarios, it's wire-compatible.

---

## §5. Head block (in-memory + WAL bridge) ✅

The head block is the in-memory state of the TSDB. It is not itself a wire format — the WAL (§4) is the durability layer; the head is the live view that produces and consumes WAL records. This section specifies the **behavioural contract** that the MVP C++ and Rust impls agree on. Disagreement here = scrape diff between binaries.

### §5.1 Conceptual model

A head holds:

- A **SeriesStore**: a map `Labels → MemSeries`, where `Labels` is a canonical (sorted-by-name, deduped) label set.
- A monotonically-allocated 64-bit **SeriesRef** per series, never reused for the lifetime of the head.
- Per-series in-memory **chunks** (XOR-encoded; §3.1) holding the most recently appended samples.
- A reference to its **WAL writer** (§4) for durability of every append.

A live MemSeries has a current "tip" chunk receiving appends. Old chunks remain in memory until the head is flushed to a persistent block (§6.6) or evicted by retention policy. Samples within a series must be monotonically non-decreasing by timestamp — out-of-order writes are rejected, not reordered.

### §5.2 Append protocol

`Head::append(labels, t, v)`:

1. Look up `labels` in the SeriesStore. If absent, allocate a new ref (= `next_ref++`, starting at 1) and create a fresh MemSeries.
2. If the series was newly created in step 1, queue a `RefSeries{ref, labels}` entry for the **pending Series record**.
3. Validate `t ≥ memseries.last_t`; reject (`InvalidArgument`-style error) if not.
4. Append `(t, v)` to the tip XORChunk. If the tip is over the 1 KiB soft-cap or at 65 535 samples, cut a new tip chunk first.
5. Queue a `RefSample{ref, t, v, st=0}` entry for the **pending SamplesV2 record**.

### §5.3 Commit semantics

`Head::commit()`:

1. If `pending_series` is non-empty, encode it as one Series record (§4.7.1) and log it via the WAL writer.
2. If `pending_samples` is non-empty, encode it as one SamplesV2 record (§4.7.2) and log it.
3. fsync the current WAL segment.
4. Clear both pending buffers.

The order **must** be Series first, SamplesV2 second — replay (§5.5) walks the WAL in order and needs the series defined before the samples reference it.

A series's WAL `RefSeries` entry is emitted **at most once per head lifetime** — only on the first append that creates the series. Subsequent appends to the same series only contribute samples.

### §5.4 Close

`Head::close()` is `commit()` followed by `wal.cut()` (which fsyncs and rolls a new segment). After close, every method except destruction returns an error. The destructor calls `close()` implicitly so a forgotten close still flushes; the explicit form exists so callers can observe errors.

### §5.5 Replay on `Head::open`

When `Head::open(dir)` finds an existing `wal/` subdirectory, it replays every complete record before opening the segment writer for fresh appends:

1. For each Series record entry, call `SeriesStore::insert_with_ref(ref, labels)`. This fails if `ref` or `labels` are already claimed by an inconsistent mapping — that's a sign of a corrupt or merged WAL and aborts open.
2. For each SamplesV2 entry, look up the series by ref. Unknown ref → corruption (the writer's invariant in §5.3 was violated). Known ref → re-append `(t, v)`, which goes through the normal monotonic-timestamp check.
3. Advance `next_ref` so it stays strictly greater than every replayed ref.

Unknown record types (Tombstones, Exemplars, histograms, etc.) are silently skipped so a head written by a future version that uses additional record types can still be partially replayed by an older binary.

### §5.6 Torn-tail tolerance

The segment reader (§4.6) reports clean EOF when the last segment ends mid-record. The head's replay loop treats that as "end of valid data" — the partial record is silently dropped, all committed records survive. Anything else (CRC mismatch, torn record in a non-last segment, unknown ref in a sample) is fatal.

### §5.7 Out of scope for MVP

The following machinery is present in upstream Go but deferred:

- **Stripe locking / concurrent appenders.** MVP is single-threaded.
- **Isolation (MVCC snapshot for queries vs. writes).** Queries against a live head are unsupported in MVP; flush to a block first.
- **m-mapped head chunks** (`chunks_head/`). All chunks stay in memory until flush.
- **Exemplars / histograms / metadata** records.

### §5.8 C++ reference

- `merlion-tsdb-cpp/src/head/mem_series.cpp` — per-series state, chunk lifecycle
- `merlion-tsdb-cpp/src/head/series_store.cpp` — ref allocation, label-keyed lookup, `insert_with_ref` for replay
- `merlion-tsdb-cpp/src/head/head.cpp` — append/commit/close/open + replay loop

---

## §6. Persistent block ✅

A persistent block is an immutable on-disk directory containing a slice of time-series data. Naming, layout, and file formats below are byte-compatible with upstream Prometheus v3.x.

### §6.1 Directory layout

```
01HZ.../                       ← directory name = ULID (§6.7)
├── meta.json                  ← §6.2
├── chunks/
│   ├── 000001                 ← §6.3, 6-digit zero-padded starting at 1
│   ├── 000002
│   └── ...
├── index                      ← §6.4 + §6.5
└── tombstones                 ← §6.8 (empty file is legal)
```

### §6.2 `meta.json`

UTF-8 JSON document. Field names match upstream's struct tags exactly (camelCase). Unknown fields must be ignored; absent optional fields default to zero / empty.

```json
{
  "version": 1,
  "ulid": "01DXXFZDYD1MQW6079WK0K6EDQ",
  "minTime": 0,
  "maxTime": 7200000,
  "stats": {
    "numSamples": 102,
    "numFloatSamples": 0,
    "numHistogramSamples": 0,
    "numSeries": 102,
    "numChunks": 102,
    "numTombstones": 0
  },
  "compaction": {
    "level": 1,
    "sources": ["01DXXFZDYD1MQW6079WK0K6EDQ"],
    "parents": [],
    "deletable": false,
    "failed": false,
    "hints": []
  }
}
```

- `version` is currently **1** for both v1 and v3 index-format blocks. (The 1 here refers to the meta.json schema, not the index file format — those are independent.) Readers must accept version=1 and reject any other value with a structured error.
- `compaction.level` is `1` for a head-flushed block; subsequent compactions increment by the rule in §7.2.
- `compaction.sources` is a list of ULIDs identifying the original leaf blocks. For a head-flush block it is `[self_ulid]`; for compacted blocks it's the union of every input's `sources` (deduplicated, lex-sorted for determinism).

Writes must be atomic: write to a `.tmp` file, then `rename()` over the target, then fsync the containing directory. (Crashed mid-write must never leave a half-formed `meta.json` visible.)

### §6.3 Chunks segment file (`chunks/NNNNNN`)

One or more **6-digit zero-padded** numeric files starting at `000001` (note: filename is the position + 1; the underlying ChunkRef uses 0-indexed array position — see §6.4).

```
header (8 bytes):
  bytes 0..3:     magic 0x85BD40DD (big-endian)
  byte 4:         version = 1
  bytes 5..7:     padding (zero)

for each chunk, sequentially until EOF:
  uvarint:        body_length L
  byte:           encoding (chunkenc::Encoding tag — see §3 table)
  L bytes:        body (e.g., XOR chunk including its 2-byte sample-count header)
  u32 BE:         CRC32-Castagnoli over the encoding byte + body
                  (NOT including the leading length uvarint)
```

Default segment cap is **512 MiB** (upstream's value); a new file is started when the next chunk would exceed it. **Records do not span segments**: a chunk is always wholly contained in one file.

### §6.4 `BlockChunkRef` encoding

A `BlockChunkRef` packs a 32-bit segment index and a 32-bit byte offset:

```
ref = (seq << 32) | offset
```

- `seq` is the **0-indexed position in the sorted segment list**, NOT the filename's numeric value. A block with one chunks file named `000001` has `seq = 0` for every chunk in it.
- `offset` is the byte position within that segment of the **chunk's length uvarint** (i.e., the start of the per-chunk record, before encoding + body + CRC).

> **Porting hazard:** the filename starts at 1 but the seq starts at 0. The off-by-one is intentional upstream — it lets the reader's segment array be 0-indexed natively without any filename↔index conversion.

### §6.5 Index file

Mixed-layout binary file. Sections are written in a fixed order; their absolute byte offsets are stored in a **TOC** (table of contents) at the very end of the file so the reader can seek directly to any section without parsing everything else.

#### §6.5.1 File-level layout

```
header (5 bytes):
  bytes 0..3:     magic 0xBAAAD700 (big-endian)
  byte 4:         version (1 = v1; 2 = v2; 3 = v3)
<sections in any order>
TOC (last 52 bytes of the file):
  6 × u64 BE:     section offsets (in the TOC field order; see §6.5.6)
  u32 BE:         CRC32-Castagnoli over the preceding 48 bytes
```

Readers must accept v1, v2, and v3. **Writers MUST emit v2** (matches upstream's actual output); v3 readers accept identical bytes (the difference is reader-internal — see §6.5.2).

#### §6.5.2 Symbol section

A **deduplicated, lex-sorted** list of UTF-8 strings. Every label name and every label value referenced by the series section is present here; symbol references in the series section index into this list.

```
u32 BE:           payload_length
u32 BE:           count (number of symbols)
for each symbol:
  uvarint-prefixed UTF-8 bytes
u32 BE:           CRC32C over the payload (count + symbols)
```

**Strings MUST be in strictly ascending lex order at write time.** Readers don't have to verify the ordering, but the V3 sparse-offset lookup table relies on it.

**Symbol reference conventions:**

- **V1**: refs are **absolute file offsets** pointing at the symbol's uvarint length byte.
- **V2 / V3**: refs are **0-based indices** into the symbol list. V3 readers maintain a sparse offset table (every 32nd symbol) for O(log n) lookup; V2 readers do linear scans. Both formats interpret refs the same way at write time.

#### §6.5.3 Series section

Each series entry is **wrapped with uvarint length framing** (not the u32 BE framing used elsewhere — this is one of the few sections that does):

```
for each series, padded to 16-byte alignment from the section start:
  uvarint:        payload_length
  payload (payload_length bytes):
    uvarint:      label_count k
    k × (uvarint name_ref, uvarint value_ref)
    uvarint:      chunk_count m
    if m > 0:
      first chunk:
        varint:   mint                  ← signed zigzag, absolute
        uvarint:  maxt_minus_mint
        uvarint:  chunk_ref             ← see §6.4, packed u64
      for each subsequent chunk:
        uvarint:  mint_delta            ← from previous chunk's maxt
        uvarint:  maxt_minus_mint
        varint:   ref_delta             ← signed zigzag, from previous chunk's ref
  u32 BE:         CRC32C over the payload bytes (NOT including the length uvarint)
  zero padding:   up to next 16-byte boundary
```

**Series ID = offset_in_file / 16.** Each series entry's byte offset must be a multiple of 16 so the inverse is unambiguous.

#### §6.5.4 Posting list

For each `(label_name, label_value)` pair that appears on at least one series, a posting list enumerates the matching series IDs in **strictly ascending order**.

```
4-byte alignment (zero padding before the next list, if needed)
u32 BE:           payload_length
u32 BE:           count
count × u32 BE:   series IDs, strictly ascending
u32 BE:           CRC32C over the payload (count + IDs)
```

Series IDs here are the same `byte_offset / 16` values returned by the series section (§6.5.3). With V3, the upper bound is the file size divided by 16 — well below `u32::MAX` for any realistic block.

#### §6.5.5 Postings offset table

A directory mapping `(name, value) → posting-list offset`:

```
u32 BE:           payload_length
u32 BE:           entry_count
for each entry:
  uvarint:        keycount (always 2 in v1/v2/v3)
  uvarint-prefixed UTF-8: label name
  uvarint-prefixed UTF-8: label value
  uvarint64:      offset of the posting list's length-prefix byte
u32 BE:           CRC32C over the payload
```

**Entries are written in the writer's internal iteration order, NOT sorted by (name, value).** Readers build their own lookup structure (typically a hash map keyed on `name + '\0' + value`).

#### §6.5.6 TOC

The final 52 bytes of the file:

```
u64 BE:           offset of the symbol section
u64 BE:           offset of the series section
u64 BE:           offset of label_indices section (legacy; emit 0 or current-position for V2/V3)
u64 BE:           offset of label_indices_table (legacy; emit 0 or current-position)
u64 BE:           offset of the postings section (first posting list)
u64 BE:           offset of the postings offset table
u32 BE:           CRC32C over the preceding 48 bytes
```

The two "label indices" fields are V1-era artifacts. V2/V3 writers may emit any value (typically a position pointer that points at an empty section); V2/V3 readers do not consume them.

### §6.6 Block creation protocol

A new block is built in this order:

1. Generate a fresh **ULID** (§6.7) — call it `U`.
2. `mkdir -p parent_dir/U`.
3. Write chunks: open `parent_dir/U/chunks/`, emit chunks via the §6.3 layout, collect their `BlockChunkRef`s.
4. Build the symbol table: every distinct label name + label value + the empty string `""`, sorted lex.
5. Open the index file at `parent_dir/U/index` and emit, in order: header → symbol section → series section (one entry per series, with label refs + chunk metas) → postings (one list per (name, value), sorted ascending series IDs) → postings offset table → TOC.
6. Write `meta.json` with computed stats (§6.2). `compaction.level = 1`; `compaction.sources = [U]`.
7. Write an **empty** `tombstones` file (zero bytes is legal — see §6.8).

The directory rename / atomic-create dance from upstream is optional in MVP — because the ULID is unique per call, a fresh directory cannot collide with an existing one. Implementations that want strict atomic-on-crash semantics can write to `parent_dir/U.tmp/` and rename when done.

### §6.7 ULID encoding

Block directory names are **26-character Crockford-base32 ULIDs**, byte-compatible with `github.com/oklog/ulid/v2`.

```
128 bits total = 16 bytes
bytes 0..5      : 48-bit ms-since-epoch timestamp, big-endian
bytes 6..15     : 80 random bits
```

Encoding to text:

```
Alphabet: "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  (32 chars; no I, L, O, U)
```

Treat the 128 bits as a big-endian integer; **prepend two zero bits** so the total is 130 bits = exactly 26 5-bit groups; emit MSB-first, indexing the alphabet for each 5-bit value.

- The first character is always in the range **`'0'..'7'`** (only 3 of its 5 bits are meaningful; the leading 2 are the zero-padding).
- ULIDs created more than 1 ms apart sort lex-ascending by creation time.
- Within the same millisecond, the random tail decides the ordering — practically always unique for a single host.

### §6.8 Tombstones file

Per-series deletion intervals. The MVP writes an empty file (zero bytes) — readers must treat an empty tombstones file as "no deletions" without erroring on absent magic.

A non-empty tombstones file has the upstream format (magic `0x0130BC91`, length-prefixed interval records). Reading and applying tombstones is **deferred**: queries don't consult them yet. Specification of the populated format will land when a compactor or admin-tooling feature actually emits non-empty tombstones.

### §6.9 Head chunks file (`chunks_head/`)

Distinct from §6.3 — used by upstream's `ChunkDiskMapper` to mmap head chunks before they're persisted into a block. **Out of scope for MVP.** When an implementation adds it: magic `0x0130BC91`, ref encoded as `(seq << 32) | offset`. Reference: `prometheus/tsdb/chunks/head_chunks.go`.

### §6.10 C++ reference

- `merlion-tsdb-cpp/src/block/meta.cpp` — meta.json read/write via nlohmann_json
- `merlion-tsdb-cpp/src/block/chunks.cpp` — chunks segment reader + writer
- `merlion-tsdb-cpp/src/block/index.cpp` — index reader (V1, V2, V3 dispatch)
- `merlion-tsdb-cpp/src/block/index_writer.cpp` — index writer (emits V2)
- `merlion-tsdb-cpp/src/block/ulid.cpp` — ULID generator + Crockford base32
- `merlion-tsdb-cpp/src/block/block.cpp` — orchestrator: `open`, `query`, `create_from_series`, `create_from_head`, `compact`

The end-to-end roundtrip test (`tests/block/block_writer_test.cpp::CrossValidationAggregateQueryRecoversAllSeries`) writes a 20-series block via the writer and recovers every series via the reader's postings + chunks API — the canonical proof that the two sides agree on the format.

---

## §7. Compaction ✅

### §7.1 Semantics

A compaction merges N ≥ 1 input blocks into a single output block. For each series present in any input:

- All chunks across all inputs are collected.
- Chunks are sorted by `min_time` ascending in the output.
- The chunk bytes themselves are not re-encoded — they're written verbatim into the output's chunks segment files.

For MVP, inputs are **assumed to have non-overlapping time ranges per series** (the standard "level promotion" use case). If two inputs cover the same `(series, timestamp)`, both chunks survive into the output; sample-level deduplication is a follow-up.

### §7.2 Level promotion

```
output.compaction.level   = max(input.compaction.level for each input) + 1
output.compaction.sources = sorted-unique union of all input.compaction.sources
```

A level-1 input is a head-flush block (sources = `[self_ulid]`). Compacting two level-1 inputs → level 2, sources = 2 ULIDs. Compacting that level-2 with a fresh level-1 → level 3, sources = 3 ULIDs.

### §7.3 Procedure

Same as §6.6, with the following modifications at step 5–6:

- Step 5: the symbol table, series, and postings are built by enumerating every series in every input block (deduplicated on canonical Labels).
- Step 6: `compaction.level` and `compaction.sources` are computed per §7.2, not defaulted to 1 / [self_ulid].

### §7.4 Out of scope

- Sample-level deduplication for overlapping inputs (vertical-merge).
- Re-chunking very large concatenated chunks (upstream splits them by sample count).
- Block deletion / GC after successful compaction — caller is responsible.

### §7.5 C++ reference

- `merlion-tsdb-cpp/src/block/block.cpp::Block::compact` — implementation
- `merlion-tsdb-cpp/tests/block/compact_test.cpp` — coverage including the two-round level-promotion test (lvl-1 + lvl-1 → lvl-2; lvl-2 + lvl-1 → lvl-3)

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
10. **WAL record header length is 2 bytes (u16), not 4.** Earlier spec drafts said 4 bytes. The 7-byte header is `type(1) + length-u16-BE(2) + crc32c-BE(4)`. Confirm with §4.3.
11. **WAL CRC32 is Castagnoli (CRC32C), not IEEE.** Same polynomial as Linux `block` layer / SCTP. Using the IEEE table gives valid-shaped but wrong-valued CRCs.
12. **WAL filenames are 8-digit zero-padded; chunks/ filenames are 6-digit.** And chunks/ starts at `000001` while the underlying `BlockChunkRef.seq` is 0-indexed (the filename is `seq + 1`). Off-by-one trap.
13. **SamplesV2 `t_delta` is from the FIRST sample's t, not the previous sample's t.** Easy to mistype; the encoder still produces seemingly-sensible bytes that decode to garbage.
14. **Series section uses uvarint length framing, not the u32 BE framing used by other index sections.** The series payload is wrapped by `NewDecbufUvarintAt`, everything else by `NewDecbufAt`. Don't reuse the same framing helper across the two.
15. **Series IDs are `byte_offset / 16`, not raw offsets.** Every series entry pads to 16-byte alignment after its CRC. If you forget the pad, two series can end up sharing an ID.
16. **Symbol additions must be in strictly ascending lex order.** V3 readers maintain a sparse offset table that relies on monotonicity. Out-of-order writes produce a file V3 readers cannot index.
17. **Postings offset table entries are NOT pre-sorted by (name, value).** They're written in the writer's internal iteration order. Build a hash map at read time; don't binary-search.
18. **Upstream writes V2, accepts V1/V2/V3.** Emitting V3 (the version byte = 3) is technically allowed but only V3 readers handle it. Emit V2 (version byte = 2) for maximum compatibility — V2 and V3 are byte-identical at write time.
19. **ULID first character is `'0'..'7'`.** Only 3 of its 5 bits are payload; the other 2 are the synthetic zero-padding that rounds 128 ULID bits up to 130 = 26 × 5.

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
- **2026-05-22** — §3.1 promoted from Drafted to Final. C++ implementation landed in `merlion-tsdb-cpp` with 12 passing tests (Debug + ASan/UBSan).
- **2026-05-22** — §4 WAL, §5 Head, §6 Persistent block all promoted to Final. New §7 Compaction added (level promotion + source aggregation). Old §7 Tombstones folded into §6.8 (still deferred for read; MVP writes empty). Pitfalls catalogue extended to 19 items, capturing every wire-format gotcha that surfaced during the C++ port. `merlion-tsdb-cpp` carries the reference implementation with 206 passing tests (Debug + ASan/UBSan), including end-to-end roundtrip: append → Head → WAL → flush → block → query → decode → bit-level parity.
