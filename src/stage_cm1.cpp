// Stage CM1: context-mixing entropy coder for BWT output.
//
// BWT output consists of long stretches dominated by one or two symbols, so
// each symbol is coded as a short chain of binary decisions:
//
//   A: is it the previous symbol c1?                 (a "run" hit, ~1 decision)
//   B: else, is it c2, the symbol seen before c1?    (a "rank 1" hit)
//   L: else, the byte itself, MSB first, 8 decisions  (a "literal")
//
// Measured on Silesia samples: about 4 decisions per symbol instead of 8, and
// literals take ~80% of the output, so that is where modelling effort pays.
//
// Every decision is predicted by several adaptive probability tables looked
// up under different contexts; their outputs are combined by a logistic mixer
// (online gradient descent), refined by an APM/SSE stage, and the final
// probability drives the binary range coder.
//
// Compared with coding all 8 bits of every byte, the A/B decomposition makes
// the common case cheap (speed) and gives run length its own contexts
// (compression).
//
// DETERMINISM: integers only. The logistic function comes from a hard-coded
// table; there is no floating point anywhere in this file. Encoder and decoder
// run the very same Model::code() template, so they cannot drift apart.
//
// Changing ANY constant, context or update rule here changes the bitstream.
// Do that only under a new stage id (see stages.hpp).
#include <algorithm>
#include <memory>

#include "rangecoder.hpp"
#include "stages.hpp"

// Model constants are compile-time constants in normal builds. A developer
// build with -DAPX_TUNE turns them into values read from the environment
// (e.g. kLimL1=16) and records where the bits go; see tools/devtool.cpp.
#ifdef APX_TUNE
#include <cmath>
#include <cstdlib>
namespace apx::tune {
double cost[3];
unsigned long long count[3];
static int get(const char* name, int def) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : def;
}
}  // namespace apx::tune
#define APX_PARAM(name, def) static const int name = ::apx::tune::get(#name, def)
#define APX_COST(kind, bit, p)                                                          \
    do {                                                                                \
        ::apx::tune::cost[kind] -= std::log2(double((bit) ? (p) : 65536 - (p)) / 65536.0); \
        ++::apx::tune::count[kind];                                                     \
    } while (0)
#else
#define APX_PARAM(name, def) constexpr int name = def
#define APX_COST(kind, bit, p) ((void)0)
#endif

namespace apx {
namespace {

// ------------------------------------------------------------------------
// Fixed-point logistic domain.
//   "stretch" units: 1/256 nat, i.e. d = 256 * ln(p / (1 - p)).
//   probabilities: 16 bit (p / 65536).

constexpr int kDotLimit = 3071;  // |d| <= 3071  <=>  p within ~[1/163000, 1 - 1/163000]

// kSquashTab[k] = round(65536 / (1 + exp(-(32*k - 3072) / 256))), capped to 65535.
// Generated once offline; hard-coded so that every platform uses identical values.
constexpr u16 kSquashTab[193] = {
    0,     0,     1,     1,     1,     1,     1,     1,     1,     1,     1,     2,
    2,     2,     2,     3,     3,     3,     4,     4,     5,     6,     6,     7,
    8,     9,     10,    12,    13,    15,    17,    19,    22,    25,    28,    32,
    36,    41,    47,    53,    60,    68,    77,    87,    98,    111,   126,   143,
    162,   184,   208,   236,   267,   302,   342,   387,   439,   497,   562,   636,
    720,   815,   922,   1042,  1179,  1333,  1506,  1701,  1921,  2168,  2446,  2758,
    3108,  3500,  3938,  4427,  4971,  5577,  6249,  6992,  7812,  8714,  9702,  10782,
    11955, 13226, 14595, 16062, 17625, 19282, 21025, 22849, 24743, 26695, 28693, 30723,
    32768, 34813, 36843, 38841, 40793, 42687, 44511, 46254, 47911, 49474, 50941, 52310,
    53581, 54754, 55834, 56822, 57724, 58544, 59287, 59959, 60565, 61109, 61598, 62036,
    62428, 62778, 63090, 63368, 63615, 63835, 64030, 64203, 64357, 64494, 64614, 64721,
    64816, 64900, 64974, 65039, 65097, 65149, 65194, 65234, 65269, 65300, 65328, 65352,
    65374, 65393, 65410, 65425, 65438, 65449, 65459, 65468, 65476, 65483, 65489, 65495,
    65500, 65504, 65508, 65511, 65514, 65517, 65519, 65521, 65523, 65524, 65526, 65527,
    65528, 65529, 65530, 65530, 65531, 65532, 65532, 65533, 65533, 65533, 65534, 65534,
    65534, 65534, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535,
};

// d (stretch units) -> 16-bit probability, linear interpolation between table points.
constexpr int squash16(int d) {
    if (d > kDotLimit) d = kDotLimit;
    if (d < -kDotLimit) d = -kDotLimit;
    const int v = d + 3072;  // 1 .. 6143
    const int i = v >> 5;    // 0 .. 191
    const int f = v & 31;
    return (kSquashTab[i] * (32 - f) + kSquashTab[i + 1] * f + 16) >> 5;
}

struct Tables {
    i16 stretch[4096];  // 12-bit probability -> d in [-2047, 2047] (inverse of squash16)
    u32 dt[1024];       // adaptive learning rate: 2^16 * 2 / (2n + 3)  ~  1 / (n + 1.5)
    u8 runq[256];       // run length -> 4-bit class
};

constexpr Tables make_tables() {
    Tables t{};
    int pi = 0;
    for (int x = -2047; x <= 2047; ++x) {
        const int v = squash16(x) >> 4;
        for (int i = pi; i <= v; ++i) t.stretch[i] = i16(x);
        pi = v + 1;
    }
    for (int i = pi; i < 4096; ++i) t.stretch[i] = 2047;

    for (int n = 0; n < 1024; ++n) t.dt[n] = 131072u / u32(2 * n + 3);

    for (int r = 0; r < 256; ++r) {
        int q = 0;
        if (r <= 4) q = r;
        else if (r <= 6) q = 5;
        else if (r <= 8) q = 6;
        else if (r <= 12) q = 7;
        else if (r <= 16) q = 8;
        else if (r <= 24) q = 9;
        else if (r <= 32) q = 10;
        else if (r <= 48) q = 11;
        else if (r <= 64) q = 12;
        else if (r <= 96) q = 13;
        else if (r <= 128) q = 14;
        else q = 15;
        t.runq[r] = u8(q);
    }
    return t;
}

constexpr Tables kT = make_tables();


// ------------------------------------------------------------------------
// Adaptive probability slot ("state map" cell).
//   bits 31..10 : P(bit == 1), 22-bit fixed point
//   bits  9..0  : n, number of updates so far (saturates at the table's limit)
// Update: p += (bit - p) / (n + 1.5). Small limits adapt fast and forget fast;
// large limits converge to a precise stationary estimate.

constexpr u32 kSlotInit = 0x80000000u;  // p = 0.5, n = 0

inline int slot_stretch(u32 s) { return kT.stretch[s >> 20]; }

inline void slot_update(u32& s, int bit, int limit_) {
    const u32 limit = u32(limit_);
    const u32 n = s & 1023u;
    i32 p = i32(s >> 10);
    const i32 target = i32(bit) << 22;
    p += i32((i64(target - p) * i64(kT.dt[n])) >> 16);  // stays within [0, 2^22 - 1]
    s = (u32(p) << 10) | (n < limit ? n + 1 : n);
}

// ------------------------------------------------------------------------
// Logistic mixer: p = squash(sum_i w_i * x_i), weights trained online to
// minimise coding cost. One weight vector per "set" (a small context).

APX_PARAM(kMixShift, 15);            // learning rate: 2^(8 - kMixShift) = 1/128
constexpr i32 kWeightMax = 1 << 22;  // |w| <= 64.0 in 16.16 fixed point (also rules out overflow)
constexpr i32 kBiasInput = 256;      // constant input (1.0 nat)

template <int N>
struct Mixer {
    std::vector<i32> w;
    i32 x[N] = {};
    i32* cur = nullptr;
    int dot = 0;     // mixed prediction in stretch units, clamped to +-kDotLimit
    int pr = 32768;  // squash16(dot)

    Mixer(size_t sets, i32 w_init) : w(sets * N, w_init) {
        for (size_t s = 0; s < sets; ++s) w[s * N + (N - 1)] = 0;  // bias weight starts at 0
    }

    inline int mix(size_t set) {
        cur = &w[set * N];
        i64 sum = 0;
        for (int i = 0; i < N; ++i) sum += i64(cur[i]) * x[i];
        sum >>= 16;
        if (sum > kDotLimit) sum = kDotLimit;
        if (sum < -kDotLimit) sum = -kDotLimit;
        dot = int(sum);
        pr = squash16(dot);
        return pr;
    }

    inline void update(int bit) {
        const i32 err = (i32(bit) << 16) - pr;  // in (-65536, 65536]
        for (int i = 0; i < N; ++i) {
            const i32 nw = cur[i] + ((x[i] * err + (1 << (kMixShift - 1))) >> kMixShift);
            cur[i] = std::clamp(nw, -kWeightMax, kWeightMax);
        }
    }
};

// ------------------------------------------------------------------------
// APM / SSE: maps (context, probability) -> refined probability by
// interpolating between 33 adaptive buckets along the stretch axis.

struct Apm {
    std::vector<u16> t;
    size_t idx = 0;

    explicit Apm(size_t contexts) : t(contexts * 33) {
        for (size_t c = 0; c < contexts; ++c)
            for (int j = 0; j < 33; ++j) t[c * 33 + j] = u16(squash16((j - 16) * 128));
    }

    inline int refine(int d, size_t ctx) {
        if (d > 2047) d = 2047;
        if (d < -2047) d = -2047;
        const int v = d + 2048;  // 1 .. 4095
        const int lo = v >> 7;   // 0 .. 31
        const int wt = v & 127;
        const u16* p = &t[ctx * 33 + lo];
        idx = ctx * 33 + lo + (wt >> 6);  // nearer bucket gets the update
        return (p[0] * (128 - wt) + p[1] * wt) >> 7;
    }

    inline void update(int bit, int rate) {
        const int g = (bit << 16) + (bit << rate) - bit - bit;
        t[idx] = u16(int(t[idx]) + ((g - int(t[idx])) >> rate));
    }
};

APX_PARAM(kApmRate, 7);
APX_PARAM(kApmWeight, 3);  // share of the APM output in the final probability, in quarters

// Final probability handed to the range coder: a blend of mixer and APM outputs, in [1, 65535].
inline u32 final_p(int p_mix, int p_apm) {
    const int p = (p_mix * (4 - kApmWeight) + p_apm * kApmWeight + 2) >> 2;
    return u32(std::clamp(p, 1, 65535));
}

// ------------------------------------------------------------------------
// Encoder / decoder adapters: the model calls io.bit(actual_bit, p) and uses
// the returned bit. The encoder returns the bit it was given; the decoder
// ignores the argument and returns the decoded bit.

struct EncIO {
    RangeEncoder rc;
    inline int bit(int b, u32 p) {
        rc.encode(b, p);
        return b;
    }
};

struct DecIO {
    RangeDecoder rc;
    inline int bit(int, u32 p) { return rc.decode(p); }
};

// ------------------------------------------------------------------------
// The model.

// Adaptation limits (see slot_update). Sweeping each of them by 2-4x in either
// direction moves the result by less than 0.05%: the optimum is flat.
APX_PARAM(kLimA1, 60);
APX_PARAM(kLimA2, 30);
APX_PARAM(kLimB1, 30);
APX_PARAM(kLimB4, 1023);
APX_PARAM(kLimW, 255);
APX_PARAM(kLimL0, 4);
APX_PARAM(kLimL1, 30);
APX_PARAM(kLimL2, 30);
APX_PARAM(kLimL3, 30);
APX_PARAM(kLimLP, 60);

// Inputs were chosen by ablation (BENCHMARKS.md): six further tables that the
// first version had (history-of-outcomes contexts for A, three contexts for B,
// a slow order-0 table for literals) together bought 0.025% and were removed.

constexpr u32 kWindow = 32;  // sliding window for local symbol frequencies (must be a power of two <= 32)

struct Model {
    // Decision A: symbol == c1 ?
    std::vector<u32> a1, a2, a5, a6;
    Mixer<5> mix_a;
    Apm apm_a;
    // Decision B: symbol == c2 ?
    std::vector<u32> b1, b4, b5;
    Mixer<4> mix_b;
    Apm apm_b;
    // Literal bits.
    std::vector<u32> l0, l1, l2, l3, lp;
    Mixer<6> mix_l;
    Apm apm_l;

    // History. Invariant on valid streams: c1 != c2.
    u32 c1 = 0;   // previous symbol
    u32 c2 = 1;   // most recent symbol different from c1
    u32 c3 = 2;   // the one before that
    u32 run = 0;  // length of the current run of c1 (saturates at 255)
    u32 ha = 0;   // last 8 outcomes of decision A
    u32 hb = 0;   // last 8 outcomes of decision B

    // Local frequency: wcnt[s] = occurrences of s among the last kWindow symbols.
    // BWT output is locally stationary, so "how common was this symbol just now"
    // is strong evidence for decisions A and B.
    u8 win[kWindow] = {};
    u8 wcnt[256] = {};
    u32 wpos = 0;

    Model()
        : a1(16 * 256, kSlotInit), a2(65536, kSlotInit), a5(64 * 16, kSlotInit), a6(64 * 64, kSlotInit),
          mix_a(16, 16384), apm_a(256),
          b1(65536, kSlotInit), b4(4096, kSlotInit), b5(64 * 64, kSlotInit),
          mix_b(16, 16384), apm_b(256),
          l0(256, kSlotInit), l1(65536, kSlotInit), l2(65536, kSlotInit), l3(65536, kSlotInit),
          lp(1024 * 256, kSlotInit),
          mix_l(256, 16384), apm_l(4 * 256) {
        wcnt[0] = u8(kWindow);  // the window starts filled with zeros
    }

    inline void win_push(u32 s) {
        --wcnt[win[wpos]];
        win[wpos] = u8(s);
        ++wcnt[s];
        wpos = (wpos + 1) & (kWindow - 1);
    }

    // Codes one symbol. Encoding: `sym` is the symbol (0..255). Decoding: `sym`
    // is ignored (pass any value >= 256). Returns the coded symbol.
    template <class IO>
    inline u32 code(IO& io, u32 sym) {
        const u32 rq = kT.runq[run];
        const u32 n1 = wcnt[c1];  // 0..32
        const u32 n2 = wcnt[c2];

        // ---------------- decision A: same as the previous symbol?
        {
            u32& s1 = a1[(rq << 8) | c1];   // run length class, symbol
            u32& s2 = a2[(c1 << 8) | c2];   // the two most recent distinct symbols
            u32& s3 = a5[(n1 << 4) | rq];   // local frequency of c1, run length class
            u32& s4 = a6[(n1 << 6) | n2];   // local frequencies of c1 and c2
            mix_a.x[0] = slot_stretch(s1);
            mix_a.x[1] = slot_stretch(s2);
            mix_a.x[2] = slot_stretch(s3);
            mix_a.x[3] = slot_stretch(s4);
            mix_a.x[4] = kBiasInput;
            const int pm = mix_a.mix(rq);
            const int pa = apm_a.refine(mix_a.dot, c1);
            const u32 p = final_p(pm, pa);
            const int a = io.bit(sym == c1, p);
            APX_COST(0, a, p);
            slot_update(s1, a, kLimA1);
            slot_update(s2, a, kLimA2);
            slot_update(s3, a, kLimW);
            slot_update(s4, a, kLimW);
            mix_a.update(a);
            apm_a.update(a, kApmRate);
            ha = ((ha << 1) | u32(a)) & 255u;
            if (a) {
                if (run < 255) ++run;
                win_push(c1);
                return c1;
            }
        }

        // ---------------- decision B: same as the symbol before the run?
        int b;
        {
            u32& s1 = b1[(c1 << 8) | c2];
            u32& s2 = b4[(hb << 4) | ((ha >> 1) & 15u)];  // recent outcomes of B and A
            u32& s3 = b5[(n2 << 6) | n1];                 // local frequencies of c2 and c1
            mix_b.x[0] = slot_stretch(s1);
            mix_b.x[1] = slot_stretch(s2);
            mix_b.x[2] = slot_stretch(s3);
            mix_b.x[3] = kBiasInput;
            const int pm = mix_b.mix(rq);
            const int pa = apm_b.refine(mix_b.dot, c2);
            const u32 p = final_p(pm, pa);
            b = io.bit(sym == c2, p);
            APX_COST(1, b, p);
            slot_update(s1, b, kLimB1);
            slot_update(s2, b, kLimB4);
            slot_update(s3, b, kLimW);
            mix_b.update(b);
            apm_b.update(b, kApmRate);
            hb = ((hb << 1) | u32(b)) & 255u;
        }

        if (b) {
            const u32 s = c2;
            c2 = c1;
            c1 = s;
            run = 1;
            win_push(s);
            return s;
        }

        // ---------------- literal: 8 binary decisions, MSB first
        const u32 i1 = c1 << 8;
        const u32 i2 = c2 << 8;
        const u32 i3 = c3 << 8;
        const u32 ip = (((c1 * 0x9E3779B1u) ^ (c2 * 0x85EBCA77u)) >> 22) << 8;  // 10-bit hash of (c1, c2)
        const u32 ia = std::min(rq, 3u) << 8;
        u32 node = 1;
        for (int k = 7; k >= 0; --k) {
            u32& s0 = l0[node];        // order 0, fast adapting: the last few literals
            u32& s1 = l1[i1 | node];   // previous symbol
            u32& s2 = l2[i2 | node];   // symbol before the run
            u32& s3 = l3[i3 | node];   // third most recent distinct symbol
            u32& s4 = lp[ip | node];   // pair (c1, c2)
            mix_l.x[0] = slot_stretch(s0);
            mix_l.x[1] = slot_stretch(s1);
            mix_l.x[2] = slot_stretch(s2);
            mix_l.x[3] = slot_stretch(s3);
            mix_l.x[4] = slot_stretch(s4);
            mix_l.x[5] = kBiasInput;
            const int pm = mix_l.mix(node);
            const int pa = apm_l.refine(mix_l.dot, ia | node);
            const u32 p = final_p(pm, pa);
            const int bit = io.bit(int((sym >> k) & 1u), p);
            APX_COST(2, bit, p);
            slot_update(s0, bit, kLimL0);
            slot_update(s1, bit, kLimL1);
            slot_update(s2, bit, kLimL2);
            slot_update(s3, bit, kLimL3);
            slot_update(s4, bit, kLimLP);
            mix_l.update(bit);
            apm_l.update(bit, kApmRate);
            node = (node << 1) | u32(bit);
        }
        const u32 s = node & 255u;
        c3 = c2;
        c2 = c1;
        c1 = s;
        run = 1;
        win_push(s);
        return s;
    }
};

}  // namespace

// ------------------------------------------------------------------------

std::vector<u8> cm1_encode(std::span<const u8> in, std::vector<u8>& params) {
    put_varint(params, in.size());

    std::vector<u8> out;
    out.reserve(in.size() / 3 + 64);
    EncIO io{RangeEncoder(out)};
    auto model = std::make_unique<Model>();
    for (const u8 s : in) model->code(io, s);
    io.rc.finish();

    // The decoder zero-extends the stream, so trailing zeros carry no information.
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

std::vector<u8> cm1_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out) {
    ByteReader pr(params);
    const u64 n = pr.get_varint();
    pr.expect_end();
    if (n == 0 || n > max_out) fail("corrupt block: CM1 size out of range");

    std::vector<u8> out(static_cast<size_t>(n));
    DecIO io{RangeDecoder(in)};
    auto model = std::make_unique<Model>();
    for (size_t i = 0; i < out.size(); ++i) out[i] = u8(model->code(io, 256));
    return out;
}

}  // namespace apx
