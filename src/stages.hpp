// Stages and the per-block pipeline.
//
// A block is encoded by a chain of stages, each mapping bytes -> bytes:
//
//     raw --stage[0]--> ... --stage[k-1]--> payload
//
// The chain is written into the block header (stage id + opaque parameters),
// so the archive describes how to decode itself. The decoder applies the
// inverse stages in reverse order. An empty chain means the block is stored.
//
// Every stage has the same two entry points:
//     encode(in, params_out) -> out      (trusted input, may not fail on data)
//     decode(params, in, max_out) -> out (untrusted input, must validate)
//
// Adding a method = adding a stage id. Existing ids never change meaning;
// a changed algorithm gets a new id, so old archives stay decodable.
#pragma once

#include "common.hpp"

namespace apx {

// Stage identifiers (part of the file format, never reuse a value).
enum StageId : u8 {
    kStageBwt = 0x10,  // Burrows-Wheeler transform (libsais). params: varint primary_index
    // 0x20 and 0x21 were development versions of CM1 and are retired.
    kStageCm1 = 0x22,  // "CM1" run/rank context-mixing coder for BWT output. params: varint decoded_size
    kStageX86 = 0x30,    // x86 call/jump filter (E8/E9 transform). no params
    kStageArm64 = 0x32,  // ARM64 BL call filter. no params
};

constexpr size_t kMaxStages = 8;         // per block
constexpr size_t kMaxStageParams = 255;  // bytes of parameters per stage

struct StageDesc {
    u8 id = 0;
    std::vector<u8> params;
};

struct EncodedBlock {
    std::vector<StageDesc> stages;  // empty => stored: payload is not used, the raw bytes are written
    std::vector<u8> payload;
    bool stored() const { return stages.empty(); }
};

struct CodecOptions {
    // Decode every encoded block and compare with the original before it is
    // written to the archive ("verify on the fly").
    bool verify = true;
};

// No intermediate buffer of a block may exceed this size. This bounds decoder
// memory by the (already validated) raw block size.
inline size_t stage_buffer_limit(size_t raw_size) { return raw_size + raw_size / 8 + 1024; }

// Rough peak working memory of the pipeline for one block, used to decide how
// many blocks may be processed in parallel.
inline u64 block_memory_estimate(u64 raw_size) { return 8 * raw_size + (16u << 20); }

// Chooses and runs the pipeline for one block. Falls back to "stored" when
// the encoded form is not strictly smaller than the raw block.
EncodedBlock encode_block(std::span<const u8> raw, const CodecOptions& opt);

// Runs the inverse pipeline. Throws apx::Error on any inconsistency.
std::vector<u8> decode_block(std::span<const StageDesc> stages, std::span<const u8> payload, size_t raw_size);

// Human-readable stage name, or nullptr if the id is unknown to this build.
const char* stage_name(u8 id);

// Individual stages (exposed for unit tests and experiments).
std::vector<u8> bwt_encode(std::span<const u8> in, std::vector<u8>& params);
std::vector<u8> bwt_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out);
std::vector<u8> cm1_encode(std::span<const u8> in, std::vector<u8>& params);
std::vector<u8> cm1_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out);
std::vector<u8> x86_encode(std::span<const u8> in, std::vector<u8>& params);
std::vector<u8> x86_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out);
size_t x86_count_candidates(std::span<const u8> in);  // operands the filter would rewrite
std::vector<u8> arm64_encode(std::span<const u8> in, std::vector<u8>& params);
std::vector<u8> arm64_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out);
size_t arm64_count_candidates(std::span<const u8> in);  // BL instructions the filter would rewrite

}  // namespace apx
