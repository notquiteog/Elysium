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

## Not the package it came from

Its NUP holds two packages, and the loader takes the program from one and the ExeFS from the
other:

```
package 0: base   version 0      exefs dir 1, romfs dir 4294967295 (none)
package 1: update version 655360 exefs dir 3, romfs dir 4
NUP program from package 0, ExeFS from package 1
```

A base with no RomFS and its data in a 655360 update is what Frizti's `34efb43fcc` describes for
the loose dump of this title, so the container is read correctly. The modules it yields are not a
mismatched pair either: `EDEN_NUP_PACKAGE=base` loads the base's own 28,684,035 byte `main`
instead of the update's 29,404,104 byte one, and it fails the same way, 18 segments over both
runs. The base's first segment begins `df 01 00 00 00 70 10 64 02 a4 10 64 02`, the same shape as
the update's, and its pair of u32s is 52 bytes apart as well (40046704, 40046756).

So both packages' modules are in this format, and taking the executables from the other one is
not the answer.

## It is an LZ4 variant, not an unknown codec

`EDEN_NSO_DUMP_SEGMENTS=<dir>` writes the bytes no codec read. Decoding one as a raw LZ4 block
from its first byte produces real ARM64: `7f 23 03 d5` (`PACIASP`), `ff 43 02 d1`
(`sub sp, sp, #0x90`), `fd 7b 07 a9` (`stp x29, x30, [sp, #0x70]`) -- a function prologue, as
LZ4 literals. So the tag byte and the pair of u32s above are not a header; they are the first
sequence's token and literals, and the stream begins at byte 0.

Plain LZ4 stops after 613,679 bytes on a match offset of 0, which LZ4 forbids. Reading the
two-byte offset as **offset - 1** carries the first segment of each module to the end of its
input:

| segment | source consumed | produced | expected |
|---|---|---|---|
| `main` seg0 | 23,151,272 / 23,151,272 | 41,249,980 | 41,194,624 |
| `sdk` seg0 | 4,637,389 / 4,637,522 | 8,128,740 | 8,092,656 |
| `subsdk0` seg0 | 518,033 / 518,035 | 733,468 | 724,832 |

Consuming the whole input means the sequences are being framed correctly. Producing 0.1 to 1.2%
too much means a length rule is still slightly wrong, each sequence contributing a few bytes.
The second and third segments of every module fail on their first match, so they are framed
differently again rather than being more of the same.

The segments are one stream. `seg1` and `seg2` fail on their first match only because it reaches
back into `seg0`'s output: concatenated, a module's three segments decode end to end with no
error at all (`subsdk0`: 750,495 of 750,535 bytes consumed). So the window is shared across the
whole module rather than reset per segment.

What holds up against the hash oracle: LZ4 sequence framing, 255-continuation extensions that
include their final byte, two-byte offsets read as offset - 1, min-match 4, and overlapping
matches that repeat. Ruled out: nibble order, a 254 continuation, min-match 3/5/6, dropping the
final extension byte, forbidding overlap, capping match length, and every start offset other
than 0.

What is left is one rule that makes the output about 1.5% too long, drifting steadily rather
than at a boundary (`subsdk0` seg0 is 8,740 over by the end of its own input).

That residual may not be in the decoder at all. The segment tables of these modules overrun
their files by 980, 31 and 301 bytes, which nothing has explained, and a header read with the
wrong layout would give both a bad table and wrong expected sizes. Worth checking the NSO header
against the bytes before assuming the decoder is what is wrong.

`EDEN_NSO_DUMP_SEGMENTS` also writes a `.sha256` sidecar holding the header's hash of the
decompressed segment and its size, so a candidate decoder is checked against the bytes the
console would have produced rather than against a length. Against that oracle, nibble order,
a 254 continuation byte and min-match 3, 5 and 6 are all ruled out; only offset - 1 with
min-match 4 comes close, and it is long by 8,636 bytes on `subsdk0` seg0.

## Where that leaves it

Two readings fit the evidence, and they want different work:

- A fourth compression format, in which case the tag byte and the pair of u32s
  are its header and the overrun is a trailer the extraction drops.
- A segment table whose last entry means something other than a byte count in
  this version, in which case the data may be reachable without a new codec.

`--verify-nup` reports the container intact, so the dump is not at fault.
