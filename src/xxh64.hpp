// XXH64 (Yann Collet's xxHash, 64-bit variant), reimplemented from the public
// specification. Used as the per-block content checksum.
// Verified in tests/unit_tests.cpp against the published reference vectors.
#pragma once

#include "common.hpp"

namespace apx {

namespace xxh_detail {
constexpr u64 P1 = 0x9E3779B185EBCA87ULL;
constexpr u64 P2 = 0xC2B2AE3D27D4EB4FULL;
constexpr u64 P3 = 0x165667B19E3779F9ULL;
constexpr u64 P4 = 0x85EBCA77C2B2AE63ULL;
constexpr u64 P5 = 0x27D4EB2F165667C5ULL;

inline u64 read64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
inline u32 read32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
inline u64 round(u64 acc, u64 input) {
    acc += input * P2;
    acc = std::rotl(acc, 31);
    return acc * P1;
}
inline u64 merge_round(u64 acc, u64 val) {
    acc ^= round(0, val);
    return acc * P1 + P4;
}
}  // namespace xxh_detail

inline u64 xxh64(const u8* data, size_t len, u64 seed = 0) {
    using namespace xxh_detail;
    const u8* p = data;
    const u8* const end = data + len;
    u64 h;

    if (len >= 32) {
        const u8* const limit = end - 32;
        u64 v1 = seed + P1 + P2;
        u64 v2 = seed + P2;
        u64 v3 = seed;
        u64 v4 = seed - P1;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = merge_round(h, v1);
        h = merge_round(h, v2);
        h = merge_round(h, v3);
        h = merge_round(h, v4);
    } else {
        h = seed + P5;
    }

    h += u64(len);

    while (size_t(end - p) >= 8) {
        h ^= round(0, read64(p));
        h = std::rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (size_t(end - p) >= 4) {
        h ^= u64(read32(p)) * P1;
        h = std::rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= u64(*p) * P5;
        h = std::rotl(h, 11) * P1;
        ++p;
    }

    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

inline u64 xxh64(std::span<const u8> data, u64 seed = 0) { return xxh64(data.data(), data.size(), seed); }

}  // namespace apx
