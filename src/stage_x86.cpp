// Stage X86: reversible filter for x86/x86-64 machine code ("E8/E9 transform").
//
// CALL (E8) and JMP (E9) instructions carry a 32-bit target *relative* to the
// instruction, so calls to the same function from different places look
// different. Adding the position turns them into the same absolute value,
// which BWT then groups together.
//
// Only the low 3 bytes are changed and only when the top byte of the operand
// is 0x00 or 0xFF (a plausible near target). The test bytes are never
// modified, so the inverse is exact: the encoder walks the block backwards,
// the decoder forwards, and each sees the very bytes the other one tested.
//
//   params: none.  Size-preserving.
#include "stages.hpp"

namespace apx {

namespace {
inline bool is_candidate(const u8* p) { return (p[0] & 0xFE) == 0xE8 && ((p[4] + 1) & 0xFE) == 0; }
}  // namespace

// Number of E8/E9 operands the filter would rewrite (used to decide whether to apply it).
size_t x86_count_candidates(std::span<const u8> in) {
    size_t n = 0;
    if (in.size() < 5) return 0;
    for (size_t i = 0; i + 5 <= in.size(); ++i) n += is_candidate(in.data() + i) ? 1 : 0;
    return n;
}

std::vector<u8> x86_encode(std::span<const u8> in, std::vector<u8>& /*params*/) {
    std::vector<u8> out(in.begin(), in.end());
    if (out.size() < 5) return out;
    for (size_t i = out.size() - 5 + 1; i-- > 0;) {
        u8* p = out.data() + i;
        if (!is_candidate(p)) continue;
        const u32 a = (u32(p[1]) | (u32(p[2]) << 8) | (u32(p[3]) << 16)) + u32(i);
        p[1] = u8(a);
        p[2] = u8(a >> 8);
        p[3] = u8(a >> 16);
    }
    return out;
}

std::vector<u8> x86_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out) {
    if (!params.empty()) fail("corrupt block: unexpected X86 filter parameters");
    if (in.size() > max_out) fail("corrupt block: X86 filter size out of range");
    std::vector<u8> out(in.begin(), in.end());
    if (out.size() < 5) return out;
    for (size_t i = 0; i + 5 <= out.size(); ++i) {
        u8* p = out.data() + i;
        if (!is_candidate(p)) continue;
        const u32 a = (u32(p[1]) | (u32(p[2]) << 8) | (u32(p[3]) << 16)) - u32(i);
        p[1] = u8(a);
        p[2] = u8(a >> 8);
        p[3] = u8(a >> 16);
    }
    return out;
}

}  // namespace apx
