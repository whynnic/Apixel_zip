# Apixel v2: benchmark results

## 11 November 2026: author's PC, 1 thread (current)

Harness `bench/bench.py`, CSV `bench/results/2026-11-11_1t.csv`, tables from `bench/summary.py`. Machine: AMD Ryzen 5 8600G
(Zen 4), all tools in 1 thread, every decompression checked against the original. Versions: apixel 2.0.0-dev (`-T 1`, 32 MiB block;
`apixel-nv` is `--no-verify`), 7-Zip 26.03, xz 5.8.3, zstd 1.5.7, brotli 1.1.0, bzip3 1.5.4, zpaq 7.15.
Not installed on the PC: bsc (see the 10 November measurement below), Kanzi, paq8px. The "12 threads" part has not been taken yet.

**Bottom line: 7-Zip Ultra is beaten**: Silesia −5.5%, enwik8 −10.9%, 10 files out of 13, compression 2.6 times faster
(4.7 times without on-the-fly verification). The main rivals are now zpaq -m5 (15.1% smaller than us, 10-18 times slower),
bzip3 (on enwik8 it is 0.8% smaller than us) and bsc; for fast mode, zstd and brotli.

#### Silesia, 12 files, total

| Archiver | Size, bytes | % of original | Compress, MiB/s | Decompress, MiB/s | Memory, MiB |
|---|---:|---:|---:|---:|---:|
| zpaq-m5 | 39 113 413 | 18.46 | 0.53 | 0.5 | 840 |
| apixel | 46 086 155 | 21.75 | 5.08 | 9.8 | 272 |
| apixel-nv | 46 086 155 | 21.75 | 9.37 | 9.2 | 207 |
| bzip3-b32 | 46 686 169 | 22.03 | 12.94 | 11.2 | 198 |
| xz-9e | 48 456 004 | 22.86 | 1.81 | 110.9 | 506 |
| 7z-ultra | 48 757 480 | 23.01 | 1.99 | 77.6 | 512 |
| xz-9 | 48 795 480 | 23.02 | 2.14 | 119.1 | 506 |
| brotli-11 | 49 564 563 | 23.39 | 0.60 | 251.4 | 237 |
| zstd-22-long | 52 364 240 | 24.71 | 1.94 | 485.9 | 741 |
| zstd-19 | 52 895 350 | 24.96 | 2.46 | 623.8 | 144 |

#### enwik8

| Archiver | Size, bytes | % of original | Compress, MiB/s | Decompress, MiB/s | Memory, MiB |
|---|---:|---:|---:|---:|---:|
| zpaq-m5 | 19 625 075 | 19.63 | 0.52 | 0.5 | 966 |
| bzip3-b32 | 21 916 449 | 21.92 | 11.05 | 9.8 | 198 |
| apixel | 22 088 794 | 22.09 | 4.48 | 9.1 | 269 |
| apixel-nv | 22 088 794 | 22.09 | 7.83 | 9.1 | 205 |
| 7z-ultra | 24 799 182 | 24.80 | 1.32 | 80.4 | 993 |
| xz-9e | 24 831 648 | 24.83 | 1.05 | 115.7 | 674 |
| xz-9 | 24 865 244 | 24.87 | 0.97 | 112.5 | 674 |
| zstd-22-long | 25 272 471 | 25.27 | 1.05 | 491.6 | 829 |
| brotli-11 | 25 742 001 | 25.74 | 0.54 | 221.3 | 203 |
| zstd-19 | 26 944 227 | 26.94 | 1.75 | 491.6 | 197 |

#### Per file, bytes

| File | apixel | 7z-ultra | bzip3-b32 | zstd-22-long | apixel vs 7z-ultra |
|---|---:|---:|---:|---:|---:|
| dickens | 2 270 742 | 2 831 111 | 2 233 934 | 2 849 067 | -19.8% |
| mozilla | 15 912 536 | 13 342 812 | 15 931 982 | 14 912 855 | +19.3% |
| mr | 2 118 439 | 2 748 257 | 2 119 990 | 3 102 188 | -22.9% |
| nci | 1 195 326 | 1 741 410 | 1 366 363 | 1 578 460 | -31.4% |
| ooffice | 2 147 394 | 2 425 568 | 2 526 882 | 2 595 237 | -11.5% |
| osdb | 2 199 955 | 2 851 796 | 2 261 854 | 3 099 939 | -22.9% |
| reymont | 984 138 | 1 318 394 | 980 459 | 1 348 575 | -25.4% |
| samba | 3 904 679 | 3 760 480 | 3 918 956 | 3 865 420 | +3.8% |
| sao | 4 675 672 | 4 413 926 | 4 673 307 | 5 000 576 | +5.9% |
| webster | 6 638 660 | 8 388 852 | 6 609 482 | 8 430 552 | -20.9% |
| x-ray | 3 652 984 | 4 479 871 | 3 657 083 | 5 129 828 | -18.5% |
| xml | 385 630 | 455 003 | 405 877 | 451 543 | -15.2% |
| enwik8 | 22 088 794 | 24 799 182 | 21 916 449 | 25 272 471 | -10.9% |

Apixel is smaller than 7-Zip Ultra on 10 files out of 13.


## 10 November 2026: cloud sandbox (first measurement)

Date: 10 November 2026. Harness: `bench/bench.py`, raw data in `bench/results/*.csv`,
the tables below were assembled by `bench/summary.py`.

**Conditions.** All tools in one thread. The machine is a cloud sandbox, 1 Intel Xeon core at 2.1 GHz, 4 GB
of memory; on a desktop CPU the absolute speeds will be higher. Every decompression was checked against the original
by SHA-256. Different tools were measured in different runs; timing noise on this machine is up to 10%,
so speeds are comparable only in order of magnitude; the sizes are exact.

**Versions and flags.** apixel 2.0.0-dev (`-T 1`, 32 MiB block; `apixel-nv` is with `--no-verify`);
7-Zip 23.01 (`a -t7z -mx=9 -mmt=1`); zstd 1.5.5 (`-19 -T1`; `--ultra -22 --long=27 -T1`);
bzip3 1.4.0 (`-b 32 -j 1`); bsc 3.3.x from source (`-b32 -t -T`, second variant with `-e2`).
Not run: Kanzi, Brotli, xz, zpaq, paq8px.

### Silesia, 12 files, total

| Archiver | Size, bytes | % of original | Compress, MiB/s | Decompress, MiB/s | Memory, MiB |
|---|---:|---:|---:|---:|---:|
| apixel-nv | 46 086 155 | 21.75 | 5.99 | 7.4 | 207 |
| apixel | 46 086 155 | 21.75 | 3.48 | 7.6 | 271 |
| bzip3-b32 | 46 686 276 | 22.03 | 9.14 | 8.7 | 198 |
| bsc-b32-e2 | 46 789 246 | 22.08 | 15.95 | 17.5 | 165 |
| bsc-b32 | 47 344 692 | 22.34 | 21.71 | 31.1 | 165 |
| 7z-ultra | 48 757 856 | 23.01 | 1.67 | 70.7 | 510 |
| zstd-22-long | 52 522 343 | 24.78 | 1.38 | 369.5 | 712 |
| zstd-19 | 53 024 573 | 25.02 | 1.73 | 436.5 | 144 |

### enwik8

| Archiver | Size, bytes | % of original | Compress, MiB/s | Decompress, MiB/s | Memory, MiB |
|---|---:|---:|---:|---:|---:|
| bzip3-b32 | 21 916 486 | 21.92 | 7.86 | 6.8 | 198 |
| bsc-b32-e2 | 21 983 674 | 21.98 | 14.06 | 16.7 | 191 |
| apixel-nv | 22 088 794 | 22.09 | 5.99 | 7.5 | 205 |
| apixel | 22 088 794 | 22.09 | 3.30 | 7.6 | 269 |
| bsc-b32 | 22 137 544 | 22.14 | 18.54 | 28.7 | 191 |
| 7z-ultra | 24 862 653 | 24.86 | 0.77 | 74.3 | 677 |
| zstd-22-long | 25 333 695 | 25.33 | 0.77 | 400.7 | 772 |
| zstd-19 | 26 954 633 | 26.95 | 1.04 | 361.2 | 188 |

### Per file, bytes

| File | apixel | 7z-ultra | bzip3-b32 | bsc-b32-e2 | zstd-22-long | apixel vs 7z-ultra |
|---|---:|---:|---:|---:|---:|---:|
| dickens | 2 270 742 | 2 831 143 | 2 233 954 | 2 259 134 | 2 849 381 | -19.8% |
| mozilla | 15 912 536 | 13 342 844 | 15 931 982 | 16 001 774 | 14 967 572 | +19.3% |
| mr | 2 118 439 | 2 748 281 | 2 119 990 | 2 206 012 | 3 105 643 | -22.9% |
| nci | 1 195 326 | 1 741 442 | 1 366 363 | 1 210 336 | 1 610 427 | -31.4% |
| ooffice | 2 147 394 | 2 425 600 | 2 526 882 | 2 559 108 | 2 598 777 | -11.5% |
| osdb | 2 199 955 | 2 851 828 | 2 261 872 | 2 224 780 | 3 098 444 | -22.9% |
| reymont | 984 138 | 1 318 426 | 980 475 | 982 364 | 1 347 556 | -25.4% |
| samba | 3 904 679 | 3 760 512 | 3 918 956 | 3 909 928 | 3 876 634 | +3.8% |
| sao | 4 675 672 | 4 413 958 | 4 673 313 | 4 679 764 | 5 000 515 | +5.9% |
| webster | 6 638 660 | 8 388 884 | 6 609 522 | 6 606 282 | 8 458 469 | -20.9% |
| x-ray | 3 652 984 | 4 479 903 | 3 657 090 | 3 756 584 | 5 155 752 | -18.5% |
| xml | 385 630 | 455 035 | 405 877 | 393 180 | 453 173 | -15.3% |
| enwik8 | 22 088 794 | 24 862 653 | 21 916 486 | 21 983 674 | 25 333 695 | -11.2% |

Apixel is smaller than 7-Zip Ultra on 10 files out of 13.

## Conclusions

1. **Size.** Apixel is 5.5% smaller than 7-Zip Ultra on Silesia and 11.2% smaller on enwik8. In its class
   (BWT) it is 1.3% smaller than bzip3 and 1.5% smaller than bsc -e2 on Silesia, but on enwik8 it loses to bzip3 by 0.8%.
2. **Compression speed.** 6.0 MiB/s without on-the-fly verification and 3.5 MiB/s with it: 3.6 and 2.1 times faster
   than 7-Zip Ultra. The goal "3 times faster at the same thread count" is met only without verification.
   bsc -e2 is 2.7 times faster than Apixel, bzip3 1.5 times faster.
3. **Decompression.** 7.4 MiB/s versus 70.7 for 7-Zip. This is the weak point of the entire BWT + CM class.
4. **Where we lose to 7-Zip:** mozilla (Alpha executables), sao (a star catalog, fixed-length records),
   samba (source mixed with binary data). The percentages are in the per-file table.
5. **The profile resembles v1:** a win over 7-Zip on 10 files out of 13, with a loss on binary data
   (v1 had 10 out of 16). This supports hypothesis H1: v1 was close to BWT.

## What was tried in the CM1 model (8 files for tuning, 64 MB)

| Change | Size | Decision |
|---|---:|---|
| baseline model | 18 861 998 | - |
| adaptation limits ×0.25…×4, mixer rate, APM weight and rate | ±0.05% | the optimum is flat, left as-is |
| local-frequency contexts (32-symbol window) for decisions A and B | −0.22% | **accepted** |
| a second APM on the previous symbol | −0.07% | rejected: −17% speed |
| a second literal mixer on the previous symbol | −0.15% | rejected: −25% speed |
| direct bit-wise coding of all symbols instead of A/B/L | −0.26% | rejected: 1.7 times slower |
| a simple BCM/bzip3-style model (fixed weights + linear SSE) | ±0.00% | 1.1% better on text, 2% worse on records |
| "yes/no" decisions on 4, 8, 16 recency ranks instead of literals | +1.1…+2.4% | rejected |
| stretch precision 16 bits instead of 12 | +0.04% | rejected |
| removal of 6 tables out of 18 after ablation (outcome history for A, three B contexts, slow order 0) | +0.025% on 4 tuning files, +0.04% on all of Silesia | **accepted**: less work per decision; the speed gain on this machine is indistinguishable from noise |
| record filter `rec` (T3b, experimental branch, 2026-11-11): period detector + column transposition / with delta before BWT | sao (R=28 found correctly) +2.9% / +9.9%; mr (R=2) +4.7% / +2.9%; x-ray (R=2) +7.5% / +7.2%; mozilla (R=24) +49% / +68% | rejected: column permutation destroys BWT contexts; a 1 MiB probe correctly rejected the filter, archives were unchanged. Code is on an experimental branch |

The x86 filter (E8/E9) on the first 32 MiB of the file: ooffice −15.4%, Linux system libraries −4.7%,
mozilla +0.7% (there is no x86 code there), the rest ±0.1%. Enabled when the candidate density is at least 1 per 200 bytes.

Held-out data (did not take part in tuning): enwik8  mozilla  nci  samba  webster. Result on enwik8:
22 220 925 (first version) → 22 088 794 (current).
