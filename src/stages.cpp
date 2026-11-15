// Stage registry and the per-block pipeline.
#include "stages.hpp"

#include "analyze.hpp"

namespace apx {
namespace {

struct Stage {
    u8 id;
    const char* name;
    std::vector<u8> (*decode)(std::span<const u8> params, std::span<const u8> in, size_t max_out);
};

constexpr Stage kStages[] = {
    {kStageBwt, "bwt", bwt_decode},
    {kStageCm1, "cm1", cm1_decode},
    {kStageX86, "x86", x86_decode},
    {kStageArm64, "arm64", arm64_decode},
};

// The x86 filter is applied when at least one byte in kX86MinDensity looks like
// a call/jump operand. Measured: real x86 code has one per 35..85 bytes, other
// data one per 1400 or rarer (where the filter costs up to 0.7%).
// A threshold of 300 (>= 1 candidate per 300 bytes, ~15% machine code in a block)
// reliably enables the filter on mixed tar archives (e.g. .so + docs/sources),
// saving up to 1.3% in compressed size while preserving a wide safety margin (4.7x)
// against false positives on non-x86 architectures (e.g. Alpha in mozilla, 1/1422 B).
constexpr size_t kX86MinDensity = 300;

// The ARM64 filter is applied when at least one byte in kArm64MinDensity looks like
// a BL instruction. Measured: real ARM64 code has one BL per 28..32 bytes, while non-ARM
// data in Silesia/enwik8 has at most one per 888 bytes (and worst-case 64 KiB chunk
// has one per 123 bytes). Setting kArm64MinDensity = 80 reliably selects ARM64
// binaries while never triggering on Silesia or enwik8.
constexpr size_t kArm64MinDensity = 80;

const Stage* find_stage(u8 id) {
    for (const Stage& s : kStages)
        if (s.id == id) return &s;
    return nullptr;
}

// Bytes the stage chain occupies in the block header.
size_t chain_header_size(const std::vector<StageDesc>& stages) {
    size_t n = 0;
    for (const StageDesc& s : stages) n += 2 + s.params.size();  // id + 1-byte length + params
    return n;
}

}  // namespace

const char* stage_name(u8 id) {
    const Stage* s = find_stage(id);
    return s ? s->name : nullptr;
}

EncodedBlock encode_block(std::span<const u8> raw, const CodecOptions& opt) {
    EncodedBlock eb;
    if (raw.empty()) fail("internal error: empty block");

    // Fast-path: bypass expensive BWT and CM1 when entropy analysis indicates
    // the block is definitely incompressible (e.g. random noise or compressed data).
    if (is_incompressible_block(raw)) return eb;

    // Pipeline: [x86 or ARM64 filter] -> BWT -> CM1. This function is where analysis,
    // more filters and alternative engines plug in (see ARCHITECTURE.md).
    std::vector<StageDesc> chain;
    std::vector<u8> filtered;
    std::span<const u8> cur = raw;
    if (x86_count_candidates(raw) * kX86MinDensity >= raw.size()) {
        chain.push_back({kStageX86, {}});
        filtered = x86_encode(raw, chain.back().params);
        cur = filtered;
    } else if (u64(arm64_count_candidates(raw)) * kArm64MinDensity >= raw.size()) {
        chain.push_back({kStageArm64, {}});
        filtered = arm64_encode(raw, chain.back().params);
        cur = filtered;
    }
    chain.push_back({kStageBwt, {}});
    std::vector<u8> bwt = bwt_encode(cur, chain.back().params);
    filtered = {};  // release early
    chain.push_back({kStageCm1, {}});
    std::vector<u8> payload = cm1_encode(bwt, chain.back().params);
    bwt = {};

    // Store unless the encoded form is strictly smaller, header bytes included.
    if (payload.size() + chain_header_size(chain) >= raw.size()) return eb;

    eb.stages = std::move(chain);
    eb.payload = std::move(payload);

    if (opt.verify) {
        const std::vector<u8> back = decode_block(eb.stages, eb.payload, raw.size());
        if (back.size() != raw.size() || std::memcmp(back.data(), raw.data(), raw.size()) != 0)
            fail("internal error: verify-on-the-fly mismatch, block was NOT written (please report)");
    }
    return eb;
}

std::vector<u8> decode_block(std::span<const StageDesc> stages, std::span<const u8> payload, size_t raw_size) {
    if (stages.empty()) fail("internal error: decode_block called for a stored block");
    if (stages.size() > kMaxStages) fail("corrupt block: too many stages");

    const size_t limit = stage_buffer_limit(raw_size);
    std::vector<u8> cur;
    std::span<const u8> in = payload;
    for (size_t i = stages.size(); i-- > 0;) {
        const Stage* st = find_stage(stages[i].id);
        if (!st) fail("unsupported stage id " + std::to_string(stages[i].id) + " (archive written by a newer apixel?)");
        std::vector<u8> out = st->decode(stages[i].params, in, limit);
        cur = std::move(out);
        in = cur;
    }
    if (cur.size() != raw_size) fail("corrupt block: decoded size mismatch");
    return cur;
}

}  // namespace apx
