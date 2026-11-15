// Binary range coder with carry propagation (the LZMA "rc" construction),
// adapted to 16-bit probabilities and an exact 32x16-bit multiply.
//
//   p1 = P(bit == 1), an integer in [1, 65535] (i.e. p1 / 65536).
//
// Why this coder: BWT output is extremely skewed, so the coder must stay
// efficient when p1 is very close to 0 or 65536. The split point is computed
// as (range * p1) >> 16 in 64-bit arithmetic, so the only loss is the final
// truncation (< 1 unit of a range that is always >= 2^24).
//
// Stream conventions (part of the format of every stage that uses this coder):
//  * the encoder never emits the leading byte of the classic LZMA stream
//    (it is always zero);
//  * the decoder treats bytes past the end of the buffer as zeros, so the
//    encoder may strip trailing zero bytes.
#pragma once

#include "common.hpp"

namespace apx {

class RangeEncoder {
public:
    explicit RangeEncoder(std::vector<u8>& out) : out_(out) {}

    inline void encode(int bit, u32 p1) {
        const u32 bound = u32((u64(range_) * p1) >> 16);
        if (bit) {
            range_ = bound;
        } else {
            low_ += bound;
            range_ -= bound;
        }
        while (range_ < (1u << 24)) {
            range_ <<= 8;
            shift_low();
        }
    }

    // Flushes the coder. Must be called exactly once, after the last bit.
    void finish() {
        for (int i = 0; i < 5; ++i) shift_low();
    }

private:
    void shift_low() {
        if (u32(low_) < 0xFF000000u || (low_ >> 32) != 0) {
            const u8 carry = u8(low_ >> 32);
            u8 temp = cache_;
            do {
                if (first_) {
                    first_ = false;  // leading byte is always 0: not stored
                } else {
                    out_.push_back(u8(temp + carry));
                }
                temp = 0xFF;
            } while (--cache_size_ != 0);
            cache_ = u8(low_ >> 24);
        }
        ++cache_size_;
        low_ = (low_ & 0x00FFFFFFull) << 8;
    }

    std::vector<u8>& out_;
    u64 low_ = 0;
    u32 range_ = 0xFFFFFFFFu;
    u8 cache_ = 0;
    u64 cache_size_ = 1;
    bool first_ = true;
};

class RangeDecoder {
public:
    explicit RangeDecoder(std::span<const u8> in) : p_(in.data()), end_(in.data() + in.size()) {
        for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | next();
    }

    inline int decode(u32 p1) {
        const u32 bound = u32((u64(range_) * p1) >> 16);
        int bit;
        if (code_ < bound) {
            range_ = bound;
            bit = 1;
        } else {
            code_ -= bound;
            range_ -= bound;
            bit = 0;
        }
        while (range_ < (1u << 24)) {
            range_ <<= 8;
            code_ = (code_ << 8) | next();
        }
        return bit;
    }

private:
    // Past the end the stream is defined to continue with zeros. A truncated
    // or corrupt stream therefore decodes to garbage of the right length,
    // which the block checksum then rejects; it can never read out of bounds.
    inline u32 next() { return p_ < end_ ? u32(*p_++) : 0u; }

    const u8* p_;
    const u8* end_;
    u32 range_ = 0xFFFFFFFFu;
    u32 code_ = 0;
};

}  // namespace apx
