// Apixel v2: common types and small helpers shared by every module.
//
// Rules that hold for the whole code base:
//  * Anything that influences decoding uses integers / fixed point only.
//  * Every read from untrusted bytes goes through ByteReader (bounds-checked).
//  * Errors are reported by throwing apx::Error; nothing calls abort()/exit().
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// The on-disk format is little-endian and the code reads integers with memcpy.
static_assert(std::endian::native == std::endian::little,
              "Apixel currently supports little-endian targets only");

// Thrown for corrupt archives, I/O failures and bad arguments.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(const std::string& msg) { throw Error(msg); }

// ---------------------------------------------------------------- writers

inline void put_u8(std::vector<u8>& out, u8 v) { out.push_back(v); }

inline void put_u32le(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(u8(v >> (8 * i)));
}

inline void put_u64le(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(u8(v >> (8 * i)));
}

// LEB128: 7 bits per byte, low bits first, high bit = "more bytes follow".
inline void put_varint(std::vector<u8>& out, u64 v) {
    while (v >= 0x80) {
        out.push_back(u8(v) | 0x80);
        v >>= 7;
    }
    out.push_back(u8(v));
}

// ---------------------------------------------------------------- reader

// Bounds-checked cursor over a byte span. Any attempt to read past the end
// throws, so parsers built on it cannot run off the buffer.
class ByteReader {
public:
    explicit ByteReader(std::span<const u8> data) : p_(data.data()), end_(data.data() + data.size()) {}

    size_t remaining() const { return size_t(end_ - p_); }
    bool at_end() const { return p_ == end_; }

    u8 get_u8() {
        need(1);
        return *p_++;
    }

    u32 get_u32le() {
        need(4);
        u32 v;
        std::memcpy(&v, p_, 4);
        p_ += 4;
        return v;
    }

    u64 get_u64le() {
        need(8);
        u64 v;
        std::memcpy(&v, p_, 8);
        p_ += 8;
        return v;
    }

    // Rejects encodings longer than 10 bytes or that overflow 64 bits.
    u64 get_varint() {
        u64 v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const u8 b = get_u8();
            if (shift == 63 && (b & 0xFE) != 0) fail("corrupt data: varint overflows 64 bits");
            v |= u64(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        fail("corrupt data: varint too long");
    }

    std::span<const u8> get_bytes(size_t n) {
        need(n);
        std::span<const u8> s(p_, n);
        p_ += n;
        return s;
    }

    void expect_end() const {
        if (p_ != end_) fail("corrupt data: unexpected trailing bytes");
    }

private:
    void need(size_t n) const {
        if (size_t(end_ - p_) < n) fail("corrupt data: unexpected end of buffer");
    }

    const u8* p_;
    const u8* end_;
};

}  // namespace apx
