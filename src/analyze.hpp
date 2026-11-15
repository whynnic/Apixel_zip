// Block compressibility analysis.
//
// Fast pre-filter to detect incompressible blocks (e.g. random data, already
// compressed archives, encrypted streams) before launching BWT.
//
// All calculations are done purely with integer arithmetic (fixed-point Q16),
// obeying the iron rule: no float or double in src/.
#pragma once

#include "common.hpp"

namespace apx {

// Returns true if the block is definitely incompressible and can be stored
// directly without running BWT and CM1.
//
// Analysis samples 16 windows of 4 KiB (64 KiB total) distributed uniformly
// across the block. A block is declared incompressible only when every sampled
// window and the aggregate sample exceed conservative Order-0 and Order-1
// entropy thresholds with ample safety margin.
bool is_incompressible_block(std::span<const u8> raw);

// Helper functions exposed for unit tests:

// Fixed-point base-2 logarithm: round(log2(x) * 65536) using integer bit-by-bit
// squaring without any floating-point arithmetic. Exact for all x in [1, 65536].
u32 int_log2_q16(u32 x);

// Computes round(n * log2(n) * 65536). Uses the precomputed integer table for
// n <= 65536, and falls back to int_log2_q16 for larger n.
u64 n_log2_q16(u32 n);

// Order-0 empirical entropy in Q16 (65536 = 1.0 bit/byte, 524288 = 8.0 bits/byte).
u32 estimate_order0_entropy_q16(std::span<const u8> sample);

// Order-1 empirical conditional entropy H(X_t | X_{t-1}) in Q16.
u32 estimate_order1_entropy_q16(std::span<const u8> sample);

}  // namespace apx
