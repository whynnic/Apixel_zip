// Block compressibility analysis implementation.
//
// Detects incompressible blocks prior to BWT and CM1.
// All calculations use integer arithmetic exclusively (fixed-point Q16, where
// 65536 corresponds to 1.0, and 524288 corresponds to 8.0 bits/byte).
#include "analyze.hpp"

namespace apx {

namespace {

// Lookup table for n * log2(n) * 65536 for n in [0, 65536].
// Precomputed at startup to replace repeated integer logarithm calculations
// with single array lookups during entropy summation.
struct NLog2Table {
    u64 table[65537];
    NLog2Table() {
        table[0] = 0;
        for (u32 n = 1; n <= 65536; ++n) {
            table[n] = u64(n) * u64(int_log2_q16(n));
        }
    }
};

const NLog2Table g_nlog2;

constexpr size_t kChunkSize = 4096;
constexpr size_t kNumChunks = 16;
constexpr size_t kTotalSampleSize = kChunkSize * kNumChunks;  // 65536 bytes

// Thresholds in fixed-point Q16 (65536 = 1.0 bit/byte):
//
// In incompressible data (urandom, xz -9, gzip, encrypted blocks):
//   - Local 4 KiB chunk H0 is consistently >= 7.94 bits (520355).
//   - Aggregate 64 KiB H0 is >= 7.99 bits (523632).
//   - Aggregate 64 KiB H1 is >= 7.17 bits (469893).
//
// In compressible data across Silesia and enwik8:
//   - The least compressible file is "sao": max chunk H0 = 7.23, combined H0 = 7.53, combined H1 = 5.11.
//   - The second highest H1 is "x-ray": max chunk H0 = 5.86, combined H0 = 6.63, combined H1 = 5.54.
//   - All other 11 corpus files have chunk H0 < 6.53, combined H0 < 6.60, combined H1 < 5.09.
//
// The thresholds below provide generous safety margins:
//   - kMinChunkH0Q16: 7.70 bits/byte (504627). Sao is 0.47 bits below; all others are >= 1.1 bits below.
//   - kMinCombinedH0Q16: 7.85 bits/byte (514458). Sao is 0.32 bits below; all others are >= 1.2 bits below.
//   - kMinCombinedH1Q16: 6.80 bits/byte (445645). X-ray is 1.26 bits below; sao is 1.69 bits below.
constexpr u32 kMinChunkH0Q16 = 504627;     // round(7.70 * 65536)
constexpr u32 kMinCombinedH0Q16 = 514458;  // round(7.85 * 65536)
constexpr u32 kMinCombinedH1Q16 = 445645;  // round(6.80 * 65536)

}  // namespace

u32 int_log2_q16(u32 x) {
    if (x <= 1) return 0;
    // Normalize x into the interval [1.0, 2.0) represented as a 32-bit integer with MSB at bit 31.
    // The characteristic k is the floor of log2(x).
    const unsigned lz = unsigned(std::countl_zero(x));
    const unsigned k = 31u - lz;
    u64 y = u64(x) << lz;
    u32 frac = 0;

    // Bit-by-bit squaring: computing the binary digits of the fractional logarithm.
    // Squaring in [1.0, 2.0) doubles the logarithm; if the square exceeds 2.0 (bit 32 set),
    // the current bit of log2 is 1 and we divide by 2 to bring the value back into [1.0, 2.0).
    for (int b = 15; b >= 0; --b) {
        const u64 sq = (y * y) >> 31;
        if (sq >= (u64(1) << 32)) {
            frac |= (1u << unsigned(b));
            y = sq >> 1;
        } else {
            y = sq;
        }
    }
    // Rounding to nearest integer in Q16: check if the next fractional bit would be 1.
    const u64 sq = (y * y) >> 31;
    if (sq >= (u64(1) << 32)) {
        frac += 1;
    }
    return (u32(k) << 16) + frac;
}

u64 n_log2_q16(u32 n) {
    if (n <= 65536) {
        return g_nlog2.table[n];
    }
    // Fall back to direct fixed-point logarithm for values beyond the table size.
    return u64(n) * u64(int_log2_q16(n));
}

u32 estimate_order0_entropy_q16(std::span<const u8> sample) {
    if (sample.empty()) return 0;
    u32 c0[256] = {};
    for (u8 b : sample) c0[b]++;
    u64 s = 0;
    for (size_t i = 0; i < 256; ++i) {
        if (c0[i] > 0) s += n_log2_q16(c0[i]);
    }
    const u32 n = u32(sample.size());
    return u32(int_log2_q16(n) - u32(s / n));
}

u32 estimate_order1_entropy_q16(std::span<const u8> sample) {
    if (sample.size() <= 1) return 0;
    // 65536 bigram counters allocate 256 KiB; dynamically allocated so function stack remains small.
    std::vector<u32> c1(65536, 0);
    const size_t n_pairs = sample.size() - 1;
    u8 prev = sample[0];
    for (size_t i = 1; i < sample.size(); ++i) {
        const u8 cur = sample[i];
        c1[(size_t(prev) << 8) | size_t(cur)]++;
        prev = cur;
    }
    u64 s_pair = 0;
    u64 s_first = 0;
    for (size_t a = 0; a < 256; ++a) {
        u32 row_sum = 0;
        const size_t base = a << 8;
        for (size_t b = 0; b < 256; ++b) {
            const u32 cnt = c1[base | b];
            if (cnt > 0) {
                row_sum += cnt;
                s_pair += n_log2_q16(cnt);
            }
        }
        if (row_sum > 0) {
            s_first += n_log2_q16(row_sum);
        }
    }
    return u32((s_first - s_pair) / n_pairs);
}

bool is_incompressible_block(std::span<const u8> raw) {
    if (raw.size() < kTotalSampleSize) {
        // Small blocks (< 64 KiB) take negligible time in BWT anyway (< 1 ms);
        // avoid small-sample statistical variance by leaving them to the standard pipeline.
        return false;
    }

    // Step 1: Sample 16 windows of 4 KiB distributed evenly across the block.
    // Incompressible data is uniformly high-entropy everywhere. If even one window shows
    // lower entropy, the block likely contains compressible sections (e.g. tar archives
    // with mixed text and compressed streams), so we bail out immediately.
    // This early exit ensures that ordinary compressible files incur less than 2 microseconds overhead.
    u32 comb_c0[256] = {};
    const size_t stride_span = raw.size() - kChunkSize;
    for (size_t i = 0; i < kNumChunks; ++i) {
        const size_t pos = size_t(u64(i) * u64(stride_span) / (kNumChunks - 1));
        const u8* chunk = raw.data() + pos;

        u32 win_c0[256] = {};
        for (size_t j = 0; j < kChunkSize; ++j) {
            win_c0[chunk[j]]++;
        }

        u64 s_win = 0;
        for (size_t c = 0; c < 256; ++c) {
            if (win_c0[c] > 0) {
                s_win += n_log2_q16(win_c0[c]);
                comb_c0[c] += win_c0[c];
            }
        }
        const u32 win_h0 = u32(int_log2_q16(u32(kChunkSize)) - u32(s_win / kChunkSize));
        if (win_h0 < kMinChunkH0Q16) {
            return false;
        }
    }

    // Step 2: Check aggregate Order-0 entropy over the full 64 KiB sample.
    u64 s_comb = 0;
    for (size_t c = 0; c < 256; ++c) {
        if (comb_c0[c] > 0) {
            s_comb += n_log2_q16(comb_c0[c]);
        }
    }
    const u32 comb_h0 = u32(int_log2_q16(u32(kTotalSampleSize)) - u32(s_comb / kTotalSampleSize));
    if (comb_h0 < kMinCombinedH0Q16) {
        return false;
    }

    // Step 3: Check aggregate Order-1 conditional entropy H(X_t | X_{t-1}).
    // This catches data where symbol frequencies appear uniform (Order-0 is ~8 bits),
    // but byte transitions are correlated or predictable (e.g. cycles, LCG PRNGs).
    // Allocated dynamically only when steps 1 and 2 passed, so compressible files never allocate.
    std::vector<u16> c1(65536, u16(0));
    size_t total_pairs = 0;
    for (size_t i = 0; i < kNumChunks; ++i) {
        const size_t pos = size_t(u64(i) * u64(stride_span) / (kNumChunks - 1));
        const u8* chunk = raw.data() + pos;
        u8 prev = chunk[0];
        for (size_t j = 1; j < kChunkSize; ++j) {
            const u8 cur = chunk[j];
            c1[(size_t(prev) << 8) | size_t(cur)]++;
            prev = cur;
        }
        total_pairs += (kChunkSize - 1);
    }

    u64 s_pair = 0;
    u64 s_first = 0;
    for (size_t a = 0; a < 256; ++a) {
        u32 row_sum = 0;
        const size_t base = a << 8;
        for (size_t b = 0; b < 256; ++b) {
            const u16 cnt = c1[base | b];
            if (cnt > 0) {
                row_sum += cnt;
                s_pair += n_log2_q16(cnt);
            }
        }
        if (row_sum > 0) {
            s_first += n_log2_q16(row_sum);
        }
    }

    const u32 comb_h1 = u32((s_first - s_pair) / total_pairs);
    if (comb_h1 < kMinCombinedH1Q16) {
        return false;
    }

    return true;
}

}  // namespace apx
