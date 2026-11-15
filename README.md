# Apixel

Experimental lossless archiver: BWT + context mixing, block-parallel, deterministic.
**Status: research prototype, archive format version 0; the format will change and old archives may stop
reading. Do not use for data you cannot afford to lose.**

## Results (1 thread, Ryzen 5 8600G, 2026-11-11)

| Tool | Silesia (12 files) | enwik8 |
|---|---:|---:|
| zpaq 7.15 -m5 | 39 113 413 | 19 625 075 |
| **apixel 2.0.0-dev** | **46 086 155** | **22 088 794** |
| bzip3 1.5.4 -b32 | 46 686 169 | 21 916 449 |
| xz 5.8.3 -9e | 48 456 004 | 24 831 648 |
| 7-Zip 26.03 Ultra | 48 757 480 | 24 799 182 |
| zstd 1.5.7 -19 | 52 895 350 | 26 944 227 |

Details, speeds and what was tried and rejected: [BENCHMARKS.md](BENCHMARKS.md). Design and format: [ARCHITECTURE.md](ARCHITECTURE.md).

## Build (Linux; GCC 11+ or Clang 14+, CMake 3.16+)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPIXEL_NATIVE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`APIXEL_NATIVE=ON` adds `-march=native`; archive bytes do not depend on it, on the thread count or on the memory limit.
Sanitizers: `-DAPIXEL_SANITIZE=ON`. Fuzzing (Clang, libFuzzer): `-DAPIXEL_FUZZ=ON`, see `fuzz/`.

## Usage

```sh
apixel c file              # compress to file.apx
apixel x file.apx          # extract
apixel t file.apx          # test integrity
apixel l file.apx          # list blocks, sizes, hashes, stage chains
tar cf - dir | apixel c - -o dir.tar.apx
```

Options: `-o <file>` (`-` = stdout), `-b <MiB>` block size 1..1024 (default 32), `-T <n>` threads,
`-M <MiB>` memory limit (default 4096), `-f` overwrite, `-q` quiet, `--no-verify` skip the on-the-fly check.

## Benchmark corpora

Not in the repository. `bench/get_corpora.sh` downloads Silesia and enwik8 and checks SHA-256;
`bench/bench.py` runs the comparison (rival tools from `PATH` or `$APIXEL_TOOL_DIR`).

## License

Apixel is licensed under the Apache License 2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Third-party: `third_party/libsais`: libsais 2.12.1 by Ilya Grebnov, Apache-2.0 (`third_party/libsais/LICENSE`).
`src/xxh64.hpp` is an independent implementation of the XXH64 specification.
