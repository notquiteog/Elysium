# Pokémon Pokopia's NSO modules

Its modules do not load. This is what is known, so the next attempt does not
start from the codecs again.

## Not LZ4, not zstd, not ZBIC

All three are tried for every segment (`src/core/loader/nso.cpp`), in either
order, and all three fail. ZBIC is genuinely available: `common/zbic_compression.cpp`
builds kinnay's Atmosphere-derived zstd with `ZSTD_ZBIC_SUPPORT`, where the frame
magic becomes `0x4349425A`, the bytes `ZBIC`. No Pokopia segment carries it, nor
zstd's `0x28B52FFD`, and LZ4 returns a negative result.

The header flag does not claim otherwise: every module reports `flags 0x3f`, the
ZBIC bit clear, NSO version 0.

## What the failing segments start with

| module | first bytes | u32 at +5 | u32 at +9 | decompressed |
|---|---|---|---|---|
| `main` seg0 | `df 01 00 00 00 ...` | 41197684 | 41197736 | 41194624 |
| `subsdk0` seg0 | `cf 01 00 00 00 ...` | 724520 | 724572 | 724832 |
| `sdk` seg0 | `cf 01 00 00 00 ...` | 8093724 | 8093776 | 8092656 |

A tag byte, then `01 00 00 00`, then two u32s **exactly 52 bytes apart** in all
three. Each is within 0.01% of that segment's decompressed size without equalling
it, and the sign of the difference varies, so neither is the size itself.

## The segment table overruns the file

Segments are contiguous in every module, the first beginning just after the
header. The difference is where the last one ends:

| module | file | last segment ends | |
|---|---|---|---|
| `rtld` | 10477 | 10477 | exact, and it loads |
| `main` | 29404104 | 29405084 | 980 over |
| `subsdk0` | 750781 | 750812 | 31 over |
| `sdk` | 7327310 | 7327611 | 301 over |

Every module that fails declares a last segment longer than the bytes present;
the one that loads fits exactly. The overrun is not a constant (980, 31, 301) and
not a block size.

`rtld` loading normally is the useful constraint: whatever these modules are, it
is not a format the title uses throughout. A theory that the NSO header is being
read with the wrong layout has to explain why `rtld` parses correctly from the
same code.

## Where that leaves it

Two readings fit the evidence, and they want different work:

- A fourth compression format, in which case the tag byte and the pair of u32s
  are its header and the overrun is a trailer the extraction drops.
- A segment table whose last entry means something other than a byte count in
  this version, in which case the data may be reachable without a new codec.

`--verify-nup` reports the container intact, so the dump is not at fault.
