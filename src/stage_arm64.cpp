// Stage ARM64 — reversible filter for AArch64 machine code (BL transform).
//
// Branch with Link (BL) instructions carry a 26-bit signed immediate (imm26)
// representing the target offset in 4-byte words. In compiled binaries, calls
// to the same common functions (e.g. library routines or shared helpers) from
// different locations contain different relative offsets.
//
// By replacing the relative imm26 with the absolute target offset
// ((position / 4 + imm26) mod 2^26), repeated calls to identical targets
// become identical 32-bit words, which significantly improves BWT sorting
// and compression ratio (measured: ~29-30% smaller archive on ARM64 binaries).
//
// Unconditional branch B instructions (opcode 0x05) are intentionally omitted
// because B is predominantly used for local control flow (loops, branches);
// making local offsets absolute makes them unique and harms compression by 16-18%.
//
// AArch64 instructions are 4-byte aligned and 4 bytes long, stored little-endian.
// Because the opcode (bits 31..26 = 0x25) and imm26 (bits 25..0) occupy disjoint
// bit fields within the 32-bit word, modifying imm26 modulo 2^26 never alters
// the opcode bits. The transform is therefore exactly reversible on ANY input
// byte sequence (including lengths not divisible by 4, leaving trailing bytes intact).
//
//   params: none. Size-preserving.
#include "stages.hpp"

namespace apx {

namespace {

// Checks whether a 4-byte little-endian word represents an ARM64 BL instruction.
// Opcode bits 31..26 are 100101b (0x25), located in bits 7..2 of byte 3.
inline bool is_bl(const u8* p) {
    return (p[3] & 0xFC) == 0x94;
}

}  // namespace

// Number of BL instructions the filter would rewrite (used to decide whether to apply it).
size_t arm64_count_candidates(std::span<const u8> in) {
    size_t n = 0;
    if (in.size() < 4) return 0;
    for (size_t i = 0; i + 4 <= in.size(); i += 4) {
        if (is_bl(in.data() + i)) ++n;
    }
    return n;
}

std::vector<u8> arm64_encode(std::span<const u8> in, std::vector<u8>& /*params*/) {
    std::vector<u8> out(in.begin(), in.end());
    if (out.size() < 4) return out;

    for (size_t i = 0; i + 4 <= out.size(); i += 4) {
        u8* p = out.data() + i;
        if (!is_bl(p)) continue;

        // Read 32-bit little-endian instruction word.
        const u32 w = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
        const u32 imm = w & 0x03FFFFFF;
        const u32 pos = static_cast<u32>(i >> 2);

        // Turn relative offset (in words) into absolute target offset modulo 2^26.
        const u32 new_imm = (imm + pos) & 0x03FFFFFF;
        const u32 new_w = (w & 0xFC000000) | new_imm;

        p[0] = u8(new_w);
        p[1] = u8(new_w >> 8);
        p[2] = u8(new_w >> 16);
        p[3] = u8(new_w >> 24);
    }
    return out;
}

std::vector<u8> arm64_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out) {
    if (!params.empty()) fail("corrupt block: unexpected ARM64 filter parameters");
    if (in.size() > max_out) fail("corrupt block: ARM64 filter size out of range");

    std::vector<u8> out(in.begin(), in.end());
    if (out.size() < 4) return out;

    for (size_t i = 0; i + 4 <= out.size(); i += 4) {
        u8* p = out.data() + i;
        if (!is_bl(p)) continue;

        // Read 32-bit little-endian instruction word.
        const u32 w = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
        const u32 imm = w & 0x03FFFFFF;
        const u32 pos = static_cast<u32>(i >> 2);

        // Turn absolute target offset back into relative offset modulo 2^26.
        const u32 orig_imm = (imm - pos) & 0x03FFFFFF;
        const u32 orig_w = (w & 0xFC000000) | orig_imm;

        p[0] = u8(orig_w);
        p[1] = u8(orig_w >> 8);
        p[2] = u8(orig_w >> 16);
        p[3] = u8(orig_w >> 24);
    }
    return out;
}

}  // namespace apx
