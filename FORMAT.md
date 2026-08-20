# CCI Container Format

CCI ("CCIM") is a compressed container for original-Xbox disc images. It stores
a disc as a sequence of fixed 2048-byte logical sectors, each either stored raw
or compressed with a single LZ4 block. A random-access index maps every sector
to its byte position in the file, so any sector can be decoded without touching
the others.

This document is the reference specification. The C library in this repository
is the reference implementation; ports to other languages (C#, Rust, …) should
follow this document and validate against the same fixtures.

All integers are **little-endian**.

## 1. Constants

| Name                  | Value  | Notes                                  |
|-----------------------|--------|----------------------------------------|
| `CCI_MAGIC`           | `0x4D494343` | ASCII bytes `43 43 49 4D` = "CCIM" |
| `CCI_HEADER_SIZE`     | `32`   | Bytes                                  |
| `CCI_SECTOR_SIZE`     | `2048` | Logical (XGD) sector size              |
| `CCI_VERSION`         | `1`    | Only version currently defined         |
| `CCI_INDEX_ALIGNMENT` | `2`    | Positions are stored `>> 2` (4-byte)   |

`index_alignment` is a shift: a stored index position is multiplied by
`1 << index_alignment` (= 4) to get the real byte offset, so every block starts
on a 4-byte boundary and file offsets up to `(2^31 - 1) << 2` ≈ 8 GiB are
representable per part.

## 2. File layout

```
+-------------------+  offset 0
| Header (32 bytes) |
+-------------------+  offset 32
| Sector 0 block    |
| Sector 1 block    |
| ...               |
| Sector N-1 block  |
+-------------------+  offset = index_offset
| Index[0]          |  uint32
| Index[1]          |
| ...               |
| Index[N]          |  N+1 entries total (sentinel end offset)
+-------------------+
```

Blocks are written contiguously starting at offset 32. The index table follows
the last block, at `index_offset`.

## 3. Header (32 bytes)

| Offset | Size | Field               | Value / meaning                         |
|-------:|-----:|---------------------|-----------------------------------------|
| 0      | 4    | `magic`             | `0x4D494343`                            |
| 4      | 4    | `header_size`       | `32`                                    |
| 8      | 8    | `uncompressed_size` | Decoded size of this part, in bytes     |
| 16     | 8    | `index_offset`      | Byte offset of the index table          |
| 24     | 4    | `block_size`        | `2048`                                  |
| 28     | 1    | `version`           | `1`                                     |
| 29     | 1    | `index_alignment`   | `2`                                     |
| 30     | 2    | `reserved`          | `0`                                     |

`sector_count = uncompressed_size / 2048`. The index has `sector_count + 1`
entries.

## 4. Index

Each entry is a `uint32`:

```
position = (entry & 0x7FFFFFFF) << index_alignment   // real byte offset
lz4       = (entry & 0x80000000) != 0                 // block is LZ4-compressed
```

For sector `i`, the block occupies bytes `[position(i), position(i+1))`; its
length is `span = position(i+1) - position(i)`. Entry `N` (the sentinel) holds
the end offset of the last block, which equals `index_offset`. The `lz4` bit of
the sentinel is unused.

## 5. Sector block

**Raw (`lz4 == 0`):** exactly 2048 bytes, the sector verbatim. `span == 2048`.

**Compressed (`lz4 == 1`):**

```
+----------------+-------------------+------------------+
| pad (1 byte)   | LZ4 block         | pad zero bytes   |
+----------------+-------------------+------------------+
```

- Byte 0 is `pad`, the number of trailing zero-padding bytes (0–3).
- The LZ4 block is `comp_len = span - 1 - pad` bytes, starting at offset 1.
- Decoding: `LZ4_decompress_safe(block + 1, out, comp_len, 2048)` must return
  exactly 2048.

The padding exists only to keep the next block 4-byte aligned; `span` is always
a multiple of 4.

## 6. Encoding a sector

```
compressed = LZ4_compress_HC(sector, tmp, 2048, level = 12 /* L12_MAX */)
if compressed > 0 and compressed < 2048 - (4 + 4):   // < 2040
    pad = align_up(compressed + 1, 4) - (compressed + 1)   // 0..3
    write byte pad
    write tmp[0 .. compressed)
    write pad zero bytes
    index bit lz4 = 1
else:
    write sector[0 .. 2048)                            // raw
    index bit lz4 = 0
record position of this block (must be 4-byte aligned)
```

The reference encoder uses LZ4-HC level 12 to match XboxToolkit. Any valid LZ4
block is accepted by decoders; byte-for-byte identity with XboxToolkit output is
not guaranteed across LZ4 implementations, but the container is interoperable.

## 7. Split (multi-part) images

A disc may be split across several parts when a single file would exceed a size
limit (e.g. FATX's 4 GiB per-file cap). Each part is a **complete, independent
CCI** covering a contiguous range of sectors:

- **Naming:** `name.1.cci`, `name.2.cci`, `name.3.cci`, … A non-split image is
  just `name.cci`.
- **Discovery:** given any part `name.K.cci`, strip the numeric sub-extension to
  get `name`, then gather every `name.<digit>.cci` sibling and sort them
  numerically. `name.cci` (no numeric sub-extension) is a single-part image.
- **Stitching:** parts are concatenated at **sector** granularity. If part *p*
  has `sector_count(p)` sectors, then part 0 covers logical sectors
  `[0, s0)`, part 1 covers `[s0, s0+s1)`, and so on. To read logical sector `L`,
  find the part whose range contains `L` and decode local sector
  `L - start_sector(part)` using that part's own header and index.
- **Splitting during encode:** start a new part when adding the next sector
  would push `position + current_index_size + 2048` past the configured
  `split_point` (0 = never split).

## References

- Team-Resurgent XboxToolkit — `CCIContainerReader.cs`, `ContainerUtility.cs`
  (the C# reference this format was derived from).
