// Self-contained unit tests (no framework). Exit code 0 = all passed.
//
// The one rule these tests guard: whatever goes in must come out bit-exact,
// and damaged archives must be rejected with apx::Error — never a crash and
// never silently wrong output.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "analyze.hpp"
#include "container.hpp"
#include "rangecoder.hpp"
#include "stages.hpp"
#include "xxh64.hpp"

using namespace apx;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// Deterministic PRNG (splitmix64) so test data is identical everywhere.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed) {}
    u64 next() {
        u64 z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    u32 below(u32 n) { return u32(next() % n); }
};

// ---------------------------------------------------------------- data generators

static std::vector<u8> gen_random(size_t n, u64 seed) {
    Rng r(seed);
    std::vector<u8> v(n);
    for (auto& b : v) b = u8(r.next());
    return v;
}

static std::vector<u8> gen_text(size_t n, u64 seed) {
    static const char* words[] = {"the",  "pixel", "of",   "archive", "and",    "data", "block", "stream",
                                  "to",   "in",    "is",   "compress", "entropy", "run",  "model", "a",
                                  "bits", "that",  "with", "context",  "symbol",  "for",  "\n",    ", "};
    Rng r(seed);
    std::vector<u8> v;
    while (v.size() < n) {
        const u32 skew = r.below(24) * r.below(24) / 24;  // favours small indices
        for (const char* p = words[skew]; *p; ++p) v.push_back(u8(*p));
        v.push_back(' ');
    }
    v.resize(n);
    return v;
}

static std::vector<u8> gen_runs(size_t n, u64 seed) {
    Rng r(seed);
    std::vector<u8> v;
    while (v.size() < n) {
        const u8 sym = u8(r.below(6) * 37);
        const u32 len = 1 + r.below(r.below(8) == 0 ? 5000 : 12);
        v.insert(v.end(), len, sym);
    }
    v.resize(n);
    return v;
}

static std::vector<u8> gen_records(size_t n, u64 seed) {  // fixed-size binary records
    Rng r(seed);
    std::vector<u8> v;
    u32 counter = 1000;
    while (v.size() < n) {
        counter += r.below(4);
        for (int i = 0; i < 4; ++i) v.push_back(u8(counter >> (8 * i)));
        v.push_back(u8(r.below(3)));
        v.push_back(0);
        v.push_back(u8(r.below(200) < 190 ? 0x7F : r.below(256)));
        v.push_back(0xFF);
    }
    v.resize(n);
    return v;
}

static std::vector<u8> gen_periodic(size_t n, size_t period, u64 seed) {
    const std::vector<u8> unit = gen_random(period, seed);
    std::vector<u8> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = unit[i % period];
    return v;
}

// x86-like code: CALL rel32 instructions to a small set of targets, with filler between them.
static std::vector<u8> gen_x86(size_t n, u64 seed) {
    Rng r(seed);
    std::vector<u8> v;
    u32 targets[16];
    for (u32& t : targets) t = r.below(u32(n));
    while (v.size() + 16 < n) {
        const u32 filler = r.below(12);
        for (u32 i = 0; i < filler; ++i) v.push_back(u8(0x40 + r.below(48)));
        const u32 rel = targets[r.below(16)] - u32(v.size() + 5);
        v.push_back(r.below(4) ? 0xE8 : 0xE9);
        for (int i = 0; i < 4; ++i) v.push_back(u8(rel >> (8 * i)));
    }
    v.resize(n, 0x90);
    return v;
}

// ARM64-like code: 4-byte instructions with BL calls to a small set of targets.
static std::vector<u8> gen_arm64(size_t n, u64 seed) {
    Rng r(seed);
    std::vector<u8> v;
    u32 targets[16];
    for (u32& t : targets) t = r.below(u32(n / 4));
    while (v.size() + 16 <= n) {
        const u32 non_bl_count = 1 + r.below(6);
        for (u32 k = 0; k < non_bl_count && v.size() + 8 <= n; ++k) {
            const u32 instr = 0xD503201F ^ (u32(r.next()) & 0x7FFFFFFF);
            const u32 clean_instr = (instr & 0x03FFFFFF) | ((instr >> 26 == 0x25 ? 0x20 : (instr >> 26)) << 26);
            for (int i = 0; i < 4; ++i) v.push_back(u8(clean_instr >> (8 * i)));
        }
        if (v.size() + 4 > n) break;
        const u32 cur_word = static_cast<u32>(v.size() / 4);
        const u32 target_word = targets[r.below(16)];
        const u32 imm26 = (target_word - cur_word) & 0x03FFFFFF;
        const u32 bl_instr = (0x25u << 26) | imm26;
        for (int i = 0; i < 4; ++i) v.push_back(u8(bl_instr >> (8 * i)));
    }
    while (v.size() < n) v.push_back(0);
    return v;
}

// ---------------------------------------------------------------- helpers

static std::vector<u8> pack(const std::vector<u8>& data, size_t block, unsigned threads, StreamStats* st = nullptr,
                            bool verify = true, u64 mem_limit = u64(4) << 30) {
    std::vector<u8> arc;
    MemInput in(data);
    MemOutput out(arc);
    CompressOptions opt;
    opt.block_size = block;
    opt.threads = threads;
    opt.verify = verify;
    opt.mem_limit = mem_limit;
    const StreamStats s = compress_stream(in, out, opt);
    if (st) *st = s;
    return arc;
}

static std::vector<u8> unpack(const std::vector<u8>& arc, unsigned threads, u64 mem_limit = u64(4) << 30) {
    std::vector<u8> data;
    MemInput in(arc);
    MemOutput out(data);
    DecompressOptions opt;
    opt.threads = threads;
    opt.mem_limit = mem_limit;
    decompress_stream(in, out, opt);
    return data;
}

static void roundtrip(const char* name, const std::vector<u8>& data, size_t block) {
    StreamStats st;
    const std::vector<u8> a1 = pack(data, block, 1, &st);
    const std::vector<u8> a4 = pack(data, block, 4);
    CHECK(a1 == a4);  // output must not depend on the thread count
    CHECK(st.raw_bytes == data.size());
    CHECK(st.archive_bytes == a1.size());
    CHECK(unpack(a1, 1) == data);
    CHECK(unpack(a1, 3) == data);
    if (std::getenv("APX_TEST_VERBOSE"))
        std::fprintf(stderr, "  %-28s %9zu -> %9zu bytes, %llu block(s), %llu stored\n", name, data.size(), a1.size(),
                     static_cast<unsigned long long>(st.blocks), static_cast<unsigned long long>(st.stored_blocks));
}

// ---------------------------------------------------------------- tests

static void test_xxh64() {
    // Reference values produced by the official xxHash library.
    CHECK(xxh64(nullptr, 0) == 0xEF46DB3751D8E999ULL);
    const u8 a[] = {'a'};
    CHECK(xxh64(a, 1) == 0xD24EC4F1A98C6E5BULL);
    const u8 abc[] = {'a', 'b', 'c'};
    CHECK(xxh64(abc, 3) == 0x44BC2CF5AD770999ULL);

    std::vector<u8> pat(1000);
    for (size_t i = 0; i < pat.size(); ++i) pat[i] = u8(i * 131 + 7);
    struct Vec {
        size_t n;
        u64 h0, h1;
    };
    const Vec vecs[] = {
        {3, 0xbed43740ee6332bbULL, 0x00b77b485431ce5dULL},  {4, 0xfa212ae44b3bb23dULL, 0x60492c4bfcb70cacULL},
        {7, 0x2744460dd675d2c0ULL, 0xcdf83b729545b016ULL},  {8, 0x994b676b71ce94ddULL, 0xf424edebfea8d23fULL},
        {31, 0x6711d55e306b5d8fULL, 0xfa1259cc8b20eb58ULL}, {32, 0x07f7b8e3bc5d6e25ULL, 0x920f3e10a5db09c6ULL},
        {33, 0x09f85eeb4e1cbe9fULL, 0x3d805e8712cb4890ULL}, {63, 0xb7c9968c066cb6a5ULL, 0x184bc5097530bb9aULL},
        {64, 0x50d4159a0411632eULL, 0x94667e10d68b3991ULL}, {100, 0x9ddada11d3dc2d8fULL, 0x5766be6782b96389ULL},
        {1000, 0x0bf0bdbcc82eb373ULL, 0xb43e358553049780ULL},
    };
    for (const Vec& v : vecs) {
        CHECK(xxh64(pat.data(), v.n, 0) == v.h0);
        CHECK(xxh64(pat.data(), v.n, 0x9E3779B1ULL) == v.h1);
    }
}

static void test_varint() {
    const u64 values[] = {0, 1, 127, 128, 255, 16383, 16384, (1ull << 32) - 1, 1ull << 32, ~0ull};
    for (const u64 v : values) {
        std::vector<u8> buf;
        put_varint(buf, v);
        ByteReader r(buf);
        CHECK(r.get_varint() == v);
        CHECK(r.at_end());
    }
    // Over-long and overflowing encodings must be rejected.
    const std::vector<u8> too_long(11, 0x80);
    bool threw = false;
    try {
        ByteReader r(too_long);
        r.get_varint();
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
    const std::vector<u8> overflow = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
    threw = false;
    try {
        ByteReader r(overflow);
        r.get_varint();
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_rangecoder() {
    for (u64 seed = 1; seed <= 4; ++seed) {
        Rng r(seed);
        const size_t n = 20000 + size_t(seed) * 7777;
        std::vector<u32> probs(n);
        std::vector<u8> bits(n);
        for (size_t i = 0; i < n; ++i) {
            const u32 kind = r.below(4);
            u32 p = kind == 0 ? 1 + r.below(8) : kind == 1 ? 65535 - r.below(8) : 1 + r.below(65535);
            probs[i] = p;
            bits[i] = u8((r.next() & 0xFFFF) < p ? 1 : 0);
            if (r.below(50) == 0) bits[i] ^= 1;  // occasional "surprise"
        }
        std::vector<u8> buf;
        RangeEncoder enc(buf);
        for (size_t i = 0; i < n; ++i) enc.encode(bits[i], probs[i]);
        enc.finish();
        while (!buf.empty() && buf.back() == 0) buf.pop_back();  // decoder zero-extends
        RangeDecoder dec(buf);
        bool ok = true;
        for (size_t i = 0; i < n && ok; ++i) ok = dec.decode(probs[i]) == bits[i];
        CHECK(ok);
    }
}

static void test_stages() {
    // BWT alone, including the tiniest inputs.
    for (size_t n = 1; n <= 40; ++n) {
        const std::vector<u8> data = gen_text(n, n);
        std::vector<u8> params;
        const std::vector<u8> t = bwt_encode(data, params);
        CHECK(bwt_decode(params, t, n) == data);
    }
    // CM1 alone on raw (non-BWT) data of several kinds.
    const std::vector<u8> inputs[] = {gen_random(5000, 1), gen_text(40000, 2), gen_runs(40000, 3),
                                      std::vector<u8>(30000, 0), std::vector<u8>(1, 200)};
    for (const auto& data : inputs) {
        std::vector<u8> params;
        const std::vector<u8> enc = cm1_encode(data, params);
        CHECK(cm1_decode(params, enc, data.size()) == data);
    }
}

static void test_x86_filter() {
    // The filter alone must be exactly reversible on anything, including
    // overlapping E8/E9 patterns and buffers shorter than one instruction.
    for (u64 seed = 0; seed < 300; ++seed) {
        Rng r(seed + 1000);
        std::vector<u8> v(r.below(seed < 40 ? 12 : 600));
        for (auto& b : v) {
            const u32 k = r.below(5);
            b = k == 0 ? 0xE8 : k == 1 ? 0xE9 : k == 2 ? 0x00 : k == 3 ? 0xFF : u8(r.next());
        }
        std::vector<u8> params;
        const std::vector<u8> enc = x86_encode(v, params);
        CHECK(params.empty());
        CHECK(x86_decode(params, enc, enc.size()) == v);
    }
    // Code-like data must select the filter, and the filter must pay off.
    const std::vector<u8> code = gen_x86(400000, 77);
    CHECK(x86_count_candidates(code) * 200 >= code.size());
    const std::vector<u8> arc = pack(code, 1 << 20, 1);
    MemInput in(arc);
    const ArchiveInfo info = list_archive(in);
    CHECK(info.blocks.size() == 1 && info.blocks[0].stage_ids.size() == 3 &&
          info.blocks[0].stage_ids[0] == kStageX86);
    CHECK(unpack(arc, 1) == code);
    std::vector<u8> p1, p2;
    const size_t plain = cm1_encode(bwt_encode(code, p1), p2).size();
    CHECK(arc.size() < plain);
    // Text must not select it.
    const std::vector<u8> text = gen_text(100000, 78);
    const std::vector<u8> arc2 = pack(text, 1 << 20, 1);
    MemInput in2(arc2);
    CHECK(list_archive(in2).blocks[0].stage_ids.size() == 2);
    // Mixed code + text with density between 200 and 300 must also select the filter.
    std::vector<u8> mixed = gen_x86(60000, 79);
    const std::vector<u8> filler = gen_text(240000, 80);
    mixed.insert(mixed.end(), filler.begin(), filler.end());
    CHECK(x86_count_candidates(mixed) * 300 >= mixed.size());
    const std::vector<u8> arc_mix = pack(mixed, 1 << 20, 1);
    MemInput in_mix(arc_mix);
    const ArchiveInfo info_mix = list_archive(in_mix);
    CHECK(info_mix.blocks.size() == 1 && info_mix.blocks[0].stage_ids.size() == 3 &&
          info_mix.blocks[0].stage_ids[0] == kStageX86);
    CHECK(unpack(arc_mix, 1) == mixed);
}

static void test_arm64_filter() {
    // The filter alone must be exactly reversible on anything, including
    // arbitrary lengths (not multiple of 4) and arbitrary random patterns.
    for (u64 seed = 0; seed < 300; ++seed) {
        Rng r(seed + 2000);
        const size_t len = r.below(seed < 40 ? 16 : 800);
        std::vector<u8> v(len);
        for (size_t i = 0; i < len; ++i) {
            // Intentionally inject BL opcodes into byte 3 of 4-byte words
            if ((i % 4 == 3) && r.below(3) == 0) {
                v[i] = u8(0x94 | (r.below(4)));
            } else {
                v[i] = u8(r.next());
            }
        }
        std::vector<u8> params;
        const std::vector<u8> enc = arm64_encode(v, params);
        CHECK(params.empty());
        CHECK(enc.size() == v.size());
        CHECK(arm64_decode(params, enc, enc.size()) == v);

        // Invalid parameters must throw an Error.
        static const u8 bad_param[] = {0x01};
        bool threw_params = false;
        try {
            (void)arm64_decode(bad_param, enc, enc.size());
        } catch (const Error&) {
            threw_params = true;
        }
        CHECK(threw_params);

        // Decoding with max_out smaller than size must throw an Error.
        if (!enc.empty()) {
            bool threw_size = false;
            try {
                (void)arm64_decode({}, enc, enc.size() - 1);
            } catch (const Error&) {
                threw_size = true;
            }
            CHECK(threw_size);
        }
    }

    // Code-like data must select the ARM64 filter, and the filter must pay off.
    const std::vector<u8> code = gen_arm64(400000, 88);
    CHECK(arm64_count_candidates(code) * 80 >= code.size());
    const std::vector<u8> arc = pack(code, 1 << 20, 1);
    MemInput in(arc);
    const ArchiveInfo info = list_archive(in);
    CHECK(info.blocks.size() == 1 && info.blocks[0].stage_ids.size() == 3 &&
          info.blocks[0].stage_ids[0] == kStageArm64);
    CHECK(unpack(arc, 1) == code);

    // Verify significant compression ratio improvement over plain BWT+CM1.
    std::vector<u8> p1, p2;
    const size_t plain = cm1_encode(bwt_encode(code, p1), p2).size();
    CHECK(arc.size() < plain);

    // Text must not select the ARM64 filter.
    const std::vector<u8> text = gen_text(100000, 89);
    CHECK(arm64_count_candidates(text) * 80 < text.size());
    const std::vector<u8> arc2 = pack(text, 1 << 20, 1);
    MemInput in2(arc2);
    CHECK(list_archive(in2).blocks[0].stage_ids.size() == 2);
}

static void test_roundtrips() {
    roundtrip("empty", {}, 1 << 16);
    for (size_t n = 1; n <= 20; ++n) roundtrip("tiny", gen_text(n, 100 + n), 1 << 16);
    roundtrip("one byte 0x00", std::vector<u8>(1, 0), 1 << 16);
    roundtrip("zeros 300k", std::vector<u8>(300000, 0), 1 << 16);
    roundtrip("0xFF 70k", std::vector<u8>(70000, 0xFF), 1 << 20);
    roundtrip("random 200k", gen_random(200000, 7), 1 << 16);
    roundtrip("text 1M / 64k blocks", gen_text(1 << 20, 8), 1 << 16);
    roundtrip("text 1M / 1 block", gen_text(1 << 20, 8), 1 << 20);
    roundtrip("runs 600k", gen_runs(600000, 9), 1 << 18);
    roundtrip("records 500k", gen_records(500000, 10), 1 << 18);
    roundtrip("periodic p=7", gen_periodic(200000, 7, 11), 1 << 17);
    roundtrip("periodic p=4099", gen_periodic(300000, 4099, 12), 1 << 20);
    roundtrip("block boundary exact", gen_text(4 << 16, 13), 1 << 16);
    roundtrip("block boundary +1", gen_text((4 << 16) + 1, 14), 1 << 16);
    roundtrip("odd block size", gen_text(100000, 15), 33333);
    roundtrip("x86-like code", gen_x86(500000, 19), 1 << 18);
    roundtrip("arm64-like code", gen_arm64(500000, 29), 1 << 18);

    // All 256 byte values, then a mix of compressible and incompressible blocks.
    std::vector<u8> all;
    for (int rep = 0; rep < 40; ++rep)
        for (int b = 0; b < 256; ++b) all.push_back(u8(b));
    roundtrip("all byte values", all, 1 << 16);

    std::vector<u8> mixed = gen_text(1 << 16, 16);
    const std::vector<u8> rnd = gen_random(1 << 16, 17);
    mixed.insert(mixed.end(), rnd.begin(), rnd.end());
    const std::vector<u8> runs = gen_runs(1 << 16, 18);
    mixed.insert(mixed.end(), runs.begin(), runs.end());
    roundtrip("mixed text/random/runs", mixed, 1 << 16);

    // Many seeds of small random-structure inputs.
    for (u64 seed = 0; seed < 60; ++seed) {
        Rng r(seed * 977 + 5);
        const size_t n = 1 + r.below(3000);
        std::vector<u8> v(n);
        const u32 alphabet = 1 + r.below(r.below(2) ? 4 : 256);
        for (auto& b : v) b = u8(r.below(alphabet) * (r.below(3) ? 1 : 17));
        roundtrip("fuzz-small", v, 1 << 10);
    }
}

static void test_store_overhead() {
    // Incompressible data must cost only the fixed framing overhead.
    const std::vector<u8> rnd = gen_random(1 << 20, 99);
    StreamStats st;
    const std::vector<u8> arc = pack(rnd, 1 << 20, 1, &st);
    CHECK(st.blocks == 1 && st.stored_blocks == 1);
    CHECK(arc.size() <= rnd.size() + 64);  // file header 12 + block header 20 + end frame 17
    if (std::getenv("APX_TEST_VERBOSE"))
        std::fprintf(stderr, "  store overhead: %zu bytes on 1 MiB of random data\n", arc.size() - rnd.size());
}

static void test_list() {
    const std::vector<u8> data = gen_text(200000, 21);
    const std::vector<u8> arc = pack(data, 1 << 16, 2);
    MemInput in(arc);
    const ArchiveInfo info = list_archive(in);
    CHECK(info.blocks.size() == 4);
    CHECK(info.raw_bytes == data.size());
    CHECK(info.archive_bytes == arc.size());
    CHECK(info.format_major == kFormatMajor);
}

// Damaged archives: every outcome other than "Error thrown" or "exact original" is a bug.
static void test_corruption() {
    std::vector<u8> data = gen_text(60000, 31);
    const std::vector<u8> rnd = gen_random(20000, 32);
    data.insert(data.end(), rnd.begin(), rnd.end());
    const std::vector<u8> runs = gen_runs(50000, 33);
    data.insert(data.end(), runs.begin(), runs.end());
    const std::vector<u8> code = gen_x86(40000, 34);
    data.insert(data.end(), code.begin(), code.end());
    const std::vector<u8> arm_code = gen_arm64(40000, 35);
    data.insert(data.end(), arm_code.begin(), arm_code.end());
    const std::vector<u8> arc = pack(data, 1 << 15, 2);

    int rejected = 0, intact = 0, wrong = 0;
    auto attempt = [&](const std::vector<u8>& bad) {
        try {
            if (unpack(bad, 2) == data) ++intact;
            else ++wrong;
        } catch (const Error&) {
            ++rejected;
        }
    };

    Rng r(4242);
    for (int i = 0; i < 400; ++i) {  // single bit flips
        std::vector<u8> bad = arc;
        bad[r.below(u32(bad.size()))] ^= u8(1u << r.below(8));
        attempt(bad);
    }
    for (int i = 0; i < 150; ++i) {  // random byte overwrites, biased towards headers
        std::vector<u8> bad = arc;
        const u32 pos = r.below(2) ? r.below(64) : r.below(u32(bad.size()));
        bad[pos] = u8(r.next());
        attempt(bad);
    }
    for (int i = 0; i < 150; ++i) {  // truncation
        std::vector<u8> bad(arc.begin(), arc.begin() + r.below(u32(arc.size())));
        attempt(bad);
    }
    for (int i = 0; i < 50; ++i) {  // appended garbage
        std::vector<u8> bad = arc;
        const u32 extra = 1 + r.below(16);
        for (u32 k = 0; k < extra; ++k) bad.push_back(u8(r.next()));
        attempt(bad);
    }
    for (int i = 0; i < 100; ++i) {  // pure garbage behind a valid file header
        std::vector<u8> bad(arc.begin(), arc.begin() + 12);
        const u32 extra = r.below(400);
        for (u32 k = 0; k < extra; ++k) bad.push_back(u8(r.next()));
        attempt(bad);
    }
    CHECK(wrong == 0);
    CHECK(rejected > 700);
    if (std::getenv("APX_TEST_VERBOSE"))
        std::fprintf(stderr, "  corruption: %d rejected, %d harmless, %d WRONG\n", rejected, intact, wrong);

    // Stage decoders fed directly with garbage must also fail cleanly or return bytes.
    for (int i = 0; i < 200; ++i) {
        const std::vector<u8> junk = gen_random(1 + r.below(300), 5000 + u64(i));
        std::vector<u8> params;
        put_varint(params, r.below(4) ? 1 + r.below(2000) : r.next());
        try {
            (void)cm1_decode(params, junk, 4096);
        } catch (const Error&) {
        }
        try {
            (void)bwt_decode(params, junk, 4096);
        } catch (const Error&) {
        }
        try {
            (void)x86_decode({}, junk, 4096);
        } catch (const Error&) {
        }
        try {
            (void)arm64_decode({}, junk, 4096);
        } catch (const Error&) {
        }
        try {
            (void)arm64_decode(params, junk, 4096);
        } catch (const Error&) {
        }
    }
}

static void test_pipeline() {
    // 1. Archive bytes and decoded output must be identical across T = 1, 2, 3, 12
    // and across tight and generous memory limits (-M).
    std::vector<u8> data = gen_text(80000, 101);
    const std::vector<u8> rnd = gen_random(35000, 102);
    data.insert(data.end(), rnd.begin(), rnd.end());
    const std::vector<u8> runs = gen_runs(65000, 103);
    data.insert(data.end(), runs.begin(), runs.end());
    const std::vector<u8> code = gen_x86(50000, 104);
    data.insert(data.end(), code.begin(), code.end());
    const std::vector<u8> arm_code = gen_arm64(50000, 105);
    data.insert(data.end(), arm_code.begin(), arm_code.end());

    const size_t block_size = 8192;
    const u64 one_block_mem = block_memory_estimate(block_size);
    const unsigned thread_counts[] = {1, 2, 3, 12};
    // Memory limits: smaller than 1 block (tests minimum-1-block progress on compress),
    // exactly 1 block, 2 blocks, 5 blocks, and 512 MiB.
    const u64 comp_mem_limits[] = {1024, one_block_mem, 2 * one_block_mem, 5 * one_block_mem, u64(512) << 20};
    const u64 decomp_mem_limits[] = {one_block_mem, 2 * one_block_mem, 5 * one_block_mem, u64(512) << 20};

    const std::vector<u8> ref_arc = pack(data, block_size, 1);
    for (unsigned t : thread_counts) {
        for (u64 m : comp_mem_limits) {
            const std::vector<u8> arc = pack(data, block_size, t, nullptr, true, m);
            CHECK(arc == ref_arc);
        }
        for (u64 m : decomp_mem_limits) {
            const std::vector<u8> dec = unpack(ref_arc, t, m);
            CHECK(dec == data);
        }
    }

    // Decompression must reject a block whose estimated memory exceeds mem_limit.
    for (unsigned t : {1u, 4u}) {
        bool rejected = false;
        try {
            (void)unpack(ref_arc, t, one_block_mem - 1);
        } catch (const Error&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    // 2. Error in the middle of a multi-block stream (corrupted block N out of many):
    // must throw apx::Error without hanging or leaking threads, and must never write
    // any bytes from the corrupted block.
    {
        MemInput list_in(ref_arc);
        const ArchiveInfo info = list_archive(list_in);
        CHECK(info.blocks.size() >= 25);

        // Corrupt a byte inside the payload of block 12 (near the middle).
        // Find the byte offset of block 12's payload by walking past the earlier blocks.
        std::vector<u8> bad_arc = ref_arc;
        // Flip bytes near the middle of the archive (guaranteed to hit a middle block).
        const size_t mid = bad_arc.size() / 2;
        for (size_t k = 0; k < 16 && mid + k + 32 < bad_arc.size(); ++k) {
            bad_arc[mid + k] ^= 0xFF;
        }

        for (unsigned t : thread_counts) {
            for (u64 m : decomp_mem_limits) {
                std::vector<u8> partial_out;
                MemInput in(bad_arc);
                MemOutput out(partial_out);
                DecompressOptions opt;
                opt.threads = t;
                opt.mem_limit = m;
                bool threw = false;
                try {
                    decompress_stream(in, out, opt);
                } catch (const Error&) {
                    threw = true;
                }
                CHECK(threw);
                // Whatever prefix was written before the bad block must be an exact
                // block-aligned prefix of the original data (no bytes from a failed block).
                CHECK(partial_out.size() < data.size());
                CHECK(partial_out.size() % block_size == 0);
                CHECK(partial_out.empty() ||
                      std::memcmp(partial_out.data(), data.data(), partial_out.size()) == 0);
            }
        }
    }

    // 3. Many small blocks (>= 1000 blocks) through the pipeline.
    {
        const size_t small_block = 256;
        const size_t num_blocks = 1200;
        const std::vector<u8> many_data = gen_text(small_block * num_blocks + 73, 2026);
        StreamStats st1;
        const std::vector<u8> arc1 = pack(many_data, small_block, 1, &st1, false);
        CHECK(st1.blocks == num_blocks + 1);

        const u64 small_est = block_memory_estimate(small_block);
        for (unsigned t : {2u, 3u, 12u}) {
            const std::vector<u8> arc_t = pack(many_data, small_block, t, nullptr, false, 3 * small_est);
            CHECK(arc_t == arc1);
            const std::vector<u8> dec_t = unpack(arc1, t, 3 * small_est);
            CHECK(dec_t == many_data);
        }
    }
}

static void test_analyze() {
    // 1. Fixed-point logarithm: exact power-of-two values and monotonicity.
    CHECK(int_log2_q16(0) == 0);
    CHECK(int_log2_q16(1) == 0);
    CHECK(int_log2_q16(2) == 65536);
    CHECK(int_log2_q16(4) == 131072);
    CHECK(int_log2_q16(8) == 196608);
    CHECK(int_log2_q16(16) == 262144);
    CHECK(int_log2_q16(256) == 524288);
    CHECK(int_log2_q16(4096) == 786432);
    CHECK(int_log2_q16(65536) == 1048576);

    for (u32 x = 1; x < 2000; ++x) {
        CHECK(int_log2_q16(x) <= int_log2_q16(x + 1));
    }

    // 2. n * log2(n) table lookups.
    CHECK(n_log2_q16(0) == 0);
    CHECK(n_log2_q16(1) == 0);
    CHECK(n_log2_q16(2) == 131072);
    CHECK(n_log2_q16(256) == 256ULL * 524288ULL);
    CHECK(n_log2_q16(4096) == 4096ULL * 786432ULL);
    CHECK(n_log2_q16(65536) == 65536ULL * 1048576ULL);

    // 3. Order-0 entropy estimation.
    CHECK(estimate_order0_entropy_q16({}) == 0);
    CHECK(estimate_order0_entropy_q16(std::vector<u8>(1000, 0x42)) == 0);

    // Equal mixture of 2 symbols: exactly 1 bit per symbol (65536 in Q16).
    std::vector<u8> two_sym(2000);
    for (size_t i = 0; i < two_sym.size(); ++i) two_sym[i] = u8(i % 2);
    const u32 h0_two = estimate_order0_entropy_q16(two_sym);
    CHECK(h0_two >= 65530 && h0_two <= 65540);

    // All 256 byte values equally repeated: exactly 8 bits per symbol (524288 in Q16).
    std::vector<u8> all_256(65536);
    for (size_t i = 0; i < all_256.size(); ++i) all_256[i] = u8(i & 0xFF);
    CHECK(estimate_order0_entropy_q16(all_256) == 524288);

    // 4. Order-1 conditional entropy estimation.
    // Deterministic cycle: order-0 is 8 bits (524288), but order-1 is 0!
    CHECK(estimate_order1_entropy_q16(all_256) == 0);
    CHECK(estimate_order1_entropy_q16(std::vector<u8>(500, 0x77)) == 0);

    // Uniform random data has both Order-0 and Order-1 very close to 8 and 7.17 bits.
    const std::vector<u8> rnd_64k = gen_random(65536, 12345);
    const u32 rnd_h0 = estimate_order0_entropy_q16(rnd_64k);
    const u32 rnd_h1 = estimate_order1_entropy_q16(rnd_64k);
    CHECK(rnd_h0 >= 523000);  // > 7.98 bits
    CHECK(rnd_h1 >= 465000);  // > 7.09 bits

    // 5. Incompressible block classification.
    // Blocks smaller than 64 KiB are never bypassed early.
    CHECK(!is_incompressible_block(gen_random(1024, 1)));
    CHECK(!is_incompressible_block(gen_random(32768, 2)));

    // Compressible structures must NEVER be classified as incompressible.
    CHECK(!is_incompressible_block(gen_text(65536, 3)));
    CHECK(!is_incompressible_block(gen_text(1 << 20, 4)));
    CHECK(!is_incompressible_block(gen_runs(65536, 5)));
    CHECK(!is_incompressible_block(gen_runs(1 << 20, 6)));
    CHECK(!is_incompressible_block(gen_records(65536, 7)));
    CHECK(!is_incompressible_block(gen_x86(65536, 8)));
    CHECK(!is_incompressible_block(gen_x86(1 << 20, 9)));

    // Data with uniform Order-0 but deterministic transitions (cycle) must not bypass.
    std::vector<u8> cycle_data(1 << 20);
    for (size_t i = 0; i < cycle_data.size(); ++i) cycle_data[i] = u8(i % 256);
    CHECK(!is_incompressible_block(cycle_data));

    // Mixed block: mostly random, but one 4 KiB chunk contains compressible zeros.
    std::vector<u8> mixed_block = gen_random(65536, 42);
    std::memset(mixed_block.data() + 7 * 4096, 0, 4096);
    CHECK(!is_incompressible_block(mixed_block));

    // Pure random data must be recognized as incompressible.
    CHECK(is_incompressible_block(rnd_64k));
    CHECK(is_incompressible_block(gen_random(1 << 20, 999)));

    // 6. Roundtrip through pack/unpack for incompressible block.
    StreamStats st;
    const std::vector<u8> packed_rnd = pack(rnd_64k, 65536, 1, &st);
    CHECK(st.blocks == 1);
    CHECK(st.stored_blocks == 1);
    const std::vector<u8> unpacked_rnd = unpack(packed_rnd, 1);
    CHECK(unpacked_rnd == rnd_64k);
}

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"xxh64", test_xxh64},           {"varint", test_varint},         {"rangecoder", test_rangecoder},
        {"stages", test_stages},         {"x86 filter", test_x86_filter}, {"arm64 filter", test_arm64_filter},
        {"roundtrips", test_roundtrips}, {"store overhead", test_store_overhead},
        {"list", test_list},             {"corruption", test_corruption}, {"pipeline", test_pipeline},
        {"analyze", test_analyze},
    };
    for (const Test& t : tests) {
        const int before = g_failed;
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failed;
            std::fprintf(stderr, "FAIL %s: unexpected exception: %s\n", t.name, e.what());
        }
        std::fprintf(stderr, "%-16s %s\n", t.name, g_failed == before ? "ok" : "FAILED");
    }
    std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
