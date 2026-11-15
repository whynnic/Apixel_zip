# Apixel v2: architecture

Document version 0.2, 10 November 2026. It describes what is already implemented and what is planned.
Markers: **[done]**: present in the code and covered by tests; **[planned]**: the decision has been made, but there is no code yet;
**[hypothesis]**: requires an experiment.

## 0. Status

| Part | Status |
|---|---|
| CLI `c / x / t / l` | [done] |
| `.apx` 0.1 container: frames, checksums, versions, self-describing blocks | [done] |
| Store mode, on-the-fly verification, decoder memory limits | [done] |
| Engine B: BWT (libsais) + CM1 coder | [done], measured |
| x86 filter (E8/E9) with automatic enablement | [done], measured |
| Multithreading over independent blocks; the result does not depend on the thread count | [done] |
| Tests: 1194 checks, ASan + UBSan, damaged archives, CLI | [done] |
| Benchmark harness `bench/`, model-tuning tool `tools/devtool` | [done] |
| Comparison with 7-Zip, zstd, bsc, bzip3 on Silesia and enwik8 | [done], see BENCHMARKS.md |
| Comparison with Kanzi, Brotli, zpaq; the author's files; multithreaded measurements | [planned] |
| LZP, other filters, fast mode (LZ), "maximum" mode, a "coordinates of coordinates" layer | [planned] |

Headline numbers (one thread, Silesia): 46.09 MB versus 48.76 for 7-Zip Ultra (−5.5%); bzip3 and bsc -e2
are larger by 1.3% and 1.5%. Compression is 3.6 times faster than 7-Zip without on-the-fly verification, and 2.1 times faster with it.
Decompression: 7.4 MiB/s versus 70.7 for 7-Zip. Details and caveats are in [BENCHMARKS.md](BENCHMARKS.md).

**The format is not frozen.** While major = 0, archives from intermediate builds may not be readable by later ones
(they will get a clear "unsupported stage id" error, not incorrect data).

## 1. Decisions already made

**Language: C++20**, built with CMake, GCC or Clang.

| Criterion | C++20 | Rust |
|---|---|---|
| Speed, SIMD, bit-level work | equal | equal |
| Libraries (libsais, zstd are in C) | linked directly | through wrappers |
| Decoder memory safety | discipline + sanitizers + fuzzing | guaranteed by the language |
| The author can read the code | easier | harder |
| v1 was written in | C++ | - |

Rust is better only in memory safety. That shortcoming of C++ is addressed by the rules in section 8.
A second language is not being introduced.

**Dependencies**: only libsais 2.12.1 (Apache-2.0), vendored in `third_party/`. XXH64 and the range coder are our own.

## 2. Formalizing the author's idea

| Author's term | Precise counterpart |
|---|---|
| "pixel" | alphabet symbol (a byte; later, a field or a bit) |
| coordinate-reference | an LZ77 phrase `(offset, length)` or a macro-scheme directive |
| "special pixel" for a frequent string | a grammar nonterminal (SLP, Re-Pair) |
| "coordinates of coordinates" | compressing the reference stream at a second level: a collage system (a grammar + truncation `X → K[l..r]`), Re²Pair, and in the limit GSLP |

The idea is captured most accurately by a **collage system**: rules for concatenation, repetition, and cutting a substring
out of an already built rule. That is "LZ77 over rules", i.e. a reference to a reference.

What the attached theoretical study says (the primary source for this section):

1. **On ordinary stationary data, depth greater than 2 gives nothing.** After the first level, all entropy
   moves into the offsets, which are almost uniform. The second level wins only on phrase lengths,
   on the order of `n·log log n / log n` bits. Levels 3 and above are asymptotically useless.
2. **Multiple levels strictly win in three cases**: long-range repeats with a small window (the visibility radius
   grows as the product of the mean phrase lengths across levels), collections of versions of the same thing, and self-similar data.
3. **Stopping rule**: keep level `k` only if the savings in the body exceed the cost of its header
   and dictionary. This is the same MDL principle as "a filter is enabled only when a gain is proven".

Conclusion: the author's idea is not suitable as the sole engine of a universal archiver, but it has three legitimate places:

- **L1. A long-range layer in front of the blocks** [planned]: searching for repeats across the whole file (rolling hash, Prefix-Free Parsing),
  applied recursively to its own reference stream. This also fixes the main drawback of independent BWT
  blocks: the loss of repeats that cross block boundaries.
- **L2. A second level over the reference stream in LZ mode** [hypothesis]: macro-references to repeating chains of
  `(lit_len, match_len, offset)`, and a Markov cache of transitions between offsets instead of a flat `rep0..rep3`.
- **L3. A grammar engine for highly repetitive data** [hypothesis]: enabled by the analyzer when
  the repetitiveness estimate is high (logs, versions, genomes).

Experiment E3 measures the contribution of each level separately. Rejection criteria are taken from the study:
L2 is dropped if the net gain is less than 0.4% on structured data, or if decompression slows down
by more than 12%.

Caveats about the study: all formulas disappeared during copying (empty characters stand in their place); I have not yet checked the 2026
citations against the primary sources; the "hundreds of MB/s" speeds in it are estimates, not measurements.

## 3. Engine candidates

| Engine | Assessment | Decision |
|---|---|---|
| A: LZ77 + optimal parsing + ANS | best decompression speed, worse compression than BWT | [planned] `fast` mode; a place for L2 |
| B: BWT + adaptive coder | best size/speed balance on CPU; a likely relative of v1 | **[done] first** |
| C: grammar | strong only on repetitive data | [hypothesis] L1/L3 layer, not an engine |
| D: bit-wise context mixing | maximum compression, an order of magnitude slower | [planned] `max` mode |

Choosing B first is justified by the goal "no worse than v1": according to survey data, the BWT class compresses more strongly than LZMA and is faster than it.
The final choice will be made by experiment E2, not by this table.

## 4. Pipeline

A block is encoded by a chain of stages, each mapping bytes to bytes:

```
raw --stage[0]--> ... --stage[k-1]--> payload
```

The chain (stage identifier + its parameters) is recorded in the block header, so the archive describes
how to decompress itself. The decoder applies the inverse stages from the end. An empty chain means the block is stored as-is.

There are currently two chains: `bwt > cm1` and `x86 > bwt > cm1`. Stage interface (`src/stages.hpp`):

```
encode(in, params_out) -> out          // trusted input
decode(params, in, max_out) -> out     // untrusted input, must check everything
```

Identifier ranges (part of the format; values are never reused):

| Range | Purpose | Occupied |
|---|---|---|
| 0x10-0x1F | structural transforms | 0x10 `bwt`; planned: LZP, long-range layer L1 |
| 0x20-0x2F | models and entropy coders | 0x22 `cm1`; 0x20 and 0x21 are development versions, retired |
| 0x30-0x3F | filters (BCJ, delta, shuffle) | 0x30 `x86`; 0x32 `arm64` (BL, density ≥ 1 per 80 B) |
| 0x40-0x4F | recompression (Deflate, JPEG) | - |
| 0x50-0x5F | LZ engines | - |
| 0x60-0x6F | grammars and "references to references" | - |
| 0xF0-0xFF | experiments, forbidden in released archives | - |

If a stage's algorithm changes, it gets a new identifier. Old archives remain readable always.

The place for analysis and chain selection is the `encode_block` function in `src/stages.cpp`. The rule for future
variants: a candidate is tried on a sample and enabled when a gain is proven; otherwise it is rolled back.

## 5. `.apx` container format, version 0.1

All numbers are little-endian. `varint` is LEB128, no longer than 10 bytes. `check` is the low 32 bits of XXH64 of all
preceding bytes of the same header.

**File header, 12 bytes**

| Field | Size | Value |
|---|---|---|
| magic | 4 | `APXL` |
| major | 1 | 0. Any other value: the reader refuses |
| minor | 1 | 1. Informational |
| block_log2 | 1 | 10..30. No block is larger than `2^block_log2` bytes |
| flags | 1 | 0. A non-zero value: the reader refuses |
| check | 4 | |

**Block frame, type 0x01**

| Field | Size | Value |
|---|---|---|
| type | 1 | 0x01 |
| raw_size | varint | 1..`2^block_log2` |
| comp_size | varint | 0..raw_size |
| nstages | 1 | 0..8. When 0 the block is stored, `comp_size == raw_size` |
| stages | nstages × (id:1, plen:1, params:plen) | in encoding order |
| hash | 8 | XXH64 of the block's original bytes |
| check | 4 | |
| payload | comp_size | |

**End frame, type 0x02**: mandatory; the end of the file must follow it.

| Field | Size | Value |
|---|---|---|
| type | 1 | 0x02 |
| total_raw | varint | sum of raw_size of all blocks |
| nblocks | varint | number of blocks |
| stream_hash | 8 | XXH64 of the concatenated hashes of all blocks; catches lost and reordered blocks |
| check | 4 | |

**Extension frames, types 0x80-0xFF**: `type, varint len, check, len bytes`. A reader that does not know the type
skips the frame. This is where file metadata, trained dictionaries, model descriptions, and the "language of bits" will go.

Stage parameters: `bwt` is a varint of the primary index (1..n); `cm1` is a varint of the result size; `x86` has no parameters.
Range-coder stream: a leading zero byte is not written, trailing zeros are trimmed, and the decoder pads the stream with zeros
(so `comp_size` may be 0: 300 KB of zeros occupy 0 bytes in the payload).

Overhead on incompressible data: 12 + 17 bytes per file and about 20 bytes per block. On 1 MiB of random
data: 49 bytes.

## 6. Engine B: BWT + CM1 [done]

BWT gathers symbols with a similar context together; after it come long runs of one or two symbols.
CM1 (`src/stage_cm1.cpp`) encodes each symbol as a short chain of binary decisions:

- **A**: is the symbol equal to the previous one, `c1`? A hit costs one decision.
- **B**: otherwise, is it equal to `c2`, the symbol that came before `c1`?
- **L**: otherwise the byte itself, 8 decisions from the most significant bit (a "literal").

Each decision is predicted by several adaptive probability tables under different contexts:
the length of the current run, the pair `c1,c2`, a third recent symbol `c3`, and for A and B, a local frequency:
how many times `c1` and `c2` occurred among the last 32 symbols. There are 12 tables in total: 4 for A, 3 for B,
5 for literals; 6 more tables from the first version were removed after ablation (together they contributed 0.025-0.04%).
A logistic mixer with online weight learning combines the predictions; an APM stage refines them;
the final probability goes into a binary range coder with 16-bit precision.

Measured on 8 Silesia files: 4.2 decisions per symbol on average instead of 8; literals are 35% of symbols
but 80% of the output bytes. They are what should be improved first.

The A/B/L split is our own design. Direct bit-wise coding on the same tables compresses
0.26% better, but is 1.7 times slower, so A/B/L is kept for balanced mode.
All model variants that were tried, with numbers, are in BENCHMARKS.md.

**x86 filter** (`src/stage_x86.cpp`): in CALL/JMP instructions the relative address is replaced with an absolute one,
so identical calls become identical bytes. Only the low 3 bytes of the operand are changed, and only
when the high byte is 0x00 or 0xFF, so the inverse transform is exact. It is enabled when a block has
at least one candidate per 200 bytes [the threshold is a hypothesis; verify on a larger set].

Memory: about 8 bytes per block byte per thread when compressing with verification, plus about 3 MB for the model.

## 7. Parallelism

[done] Blocks are independent. A batch of blocks is read, they are processed in parallel, and the results are written
in order. The archive bytes depend only on the input and the block size, never on the thread count or the memory limit.
The `-M` memory limit reduces the batch size.

The cost of independent blocks: repeats between blocks are lost. [planned] This is fixed by the L1 layer (section 2) and a larger
block; [planned] a streaming pool instead of batches; [hypothesis] a multithreaded libsais for one huge block.

## 8. Safety and reliability

- **On-the-fly verification** [done]: each encoded block is decompressed and compared with the original
  before it is written. Enabled by default, disabled with `--no-verify`. The cost is the time to decompress the block.
- **Store** [done]: if the encoded block is not strictly smaller than the original, the original is written.
- **Determinism** [done]: there is no floating-point arithmetic in the codec. The logistic function is taken from
  a table baked into the code. The encoder and the decoder execute the same templated model function.
- **Untrusted input** [done]: all reads go through bounds-checking classes; sizes are checked before
  memory is allocated; a frame header is checksummed before the payload is read; intermediate buffers
  are no larger than `raw_size·1.125 + 1024`; a block that would need more memory than the limit is rejected.
- **Output** [done]: written to a temporary file and renamed on success. An existing file is not overwritten
  without `-f`.
- **Corruption test** [done]: 850 damaged archives (including blocks with the x86 filter) (bits, bytes, truncation, junk) were all either rejected
  or decompressed correctly, under ASan and UBSan with not a single hit.
- [planned] Real fuzzing (libFuzzer) of the decoder and, separately, of libsais inverse BWT on arbitrary data.

## 9. Modes

| Mode | Composition | Goal, equal thread count | Status |
|---|---|---|---|
| fast | LZ + ANS | not dominated by zstd -19 on the size/speed pair | [planned] |
| balanced | x86 + BWT + CM1 | size at least 1% smaller than 7-Zip Ultra at 3 times the compression speed | size: −5.5% ✔; speed: ×3.6 without verification ✔, ×2.1 with verification ✘ |
| max | context mixing | closer to Kanzi -l 9 than to xz | [planned] |

## 10. Training and extension [planned]

- Trained dictionaries and models are stored in extension frames and addressed by the hash of their contents;
  a block refers to them via a stage parameter. An archive without the required dictionary is not silently decompressed: it is an error.
- "Language of bits": a description of how to parse a format becomes a filter stage (field splitting), and learned
  "plans" become the choice of stage chain in `encode_block`.
- AI improvement loop: the evaluator is the total size on the corpus under speed and memory constraints, plus
  a mandatory bit-for-bit check; the score is computed on a held-out set. The first target is the CM1 constants and contexts.

## 11. Hypotheses about v1

| Hypothesis | Result |
|---|---|
| H1: v1 was close to BWT/LZP | **supported**: engine B gives the same profile: a win over 7-Zip on 10 files out of 13 (v1 had 10 out of 16), a loss on binary data, and a failure on random data without store mode |
| H2: speed came from multithreading | not tested: v1 ran on a server with an unknown number of cores. Even in one thread, engine B is 3.7 times faster than 7-Zip |
| H3: no heavy parsing | follows from H1 |
| H4: favorable files | partial: a 16-31% gain on text and tables, a 4-19% loss on binary data |
| H5: quirks of the measurements | not tested |
| H6: 7-Zip Ultra uses few threads | not tested; a multicore machine is needed |

v1 cannot be restored exactly: the test tables are lost. Only profile compatibility is being checked.

## 12. Risks and open questions

1. Decompression is slow (7 MiB/s per core). For a "compress once, read many times" scenario, a fast mode is needed.
2. Binary data without x86 code (mozilla, sao, samba): BWT loses to LZMA by 4-19%. Segmentation and
   field permutation in bsc give no more than 2-3% on them, so a different engine (LZ) with per-block selection is needed.
3. On English text, a simple BCM/bzip3-style model is 1% better than CM1 (dickens, enwik8). The reason is not known:
   it is not the information sources, but the mixing method (linear + SSE versus logistic).
4. 32 MiB blocks lose long-range repeats. The remedy is the L1 layer.
5. The safety of inverse BWT in libsais on arbitrary input has been confirmed only by the corruption test, not by fuzzing.
6. All measurements were taken on a single core in a sandbox; they need to be repeated on a Ryzen 5 8600G and with 12 threads.

## 13. Next steps

Overall order:

1. Speeding up CM1: SIMD in the mixer and in table updates. Interleaving two to four independent streams on one
   core has been tried and does not speed things up (it even slows them down by 10-25%), but splitting a block into independent segments
   costs only 0.1-0.2% in size; this is a way to occupy all cores inside a single block.
2. Repeat the measurements on the author's machine, with Kanzi and with several threads; the author's files; the held-out set.
3. A second LZ engine (fast mode and binary data) with per-block engine selection; the second reference level L2 lives in it as well.
4. Filters: delta and field splitting for records (sao, x-ray, mr), ARM64/Alpha branches.
5. LZP before BWT; detecting incompressible blocks without running BWT.
6. The L1 layer (long-range repeats) and experiment E3.
7. Fuzzing, a streaming pool, max mode.
