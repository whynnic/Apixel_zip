// Stage BWT: Burrows-Wheeler transform of one block via libsais.
//
//   encode: in[n] -> out[n], params = varint primary_index (1..n)
//   decode: the inverse; the output has the same size as the input.
//
// Memory: about 5n on both sides (n bytes output + 4n-byte suffix/index array),
// on top of the caller's input buffer.
#include <limits>
#include <memory>

#include "libsais.h"
#include "stages.hpp"

namespace apx {

// libsais works with 32-bit indices; the container additionally caps blocks at 1 GiB.
static constexpr size_t kBwtMaxBlock = size_t(1) << 30;

std::vector<u8> bwt_encode(std::span<const u8> in, std::vector<u8>& params) {
    if (in.empty() || in.size() > kBwtMaxBlock) fail("internal error: BWT block size out of range");
    const i32 n = i32(in.size());

    std::vector<u8> out(in.size());
    auto sa = std::make_unique_for_overwrite<i32[]>(in.size());
    const i32 primary = libsais_bwt(in.data(), out.data(), sa.get(), n, 0, nullptr);
    if (primary < 0) fail("internal error: libsais_bwt failed");

    put_varint(params, u64(primary));
    return out;
}

std::vector<u8> bwt_decode(std::span<const u8> params, std::span<const u8> in, size_t max_out) {
    ByteReader pr(params);
    const u64 primary = pr.get_varint();
    pr.expect_end();

    if (in.empty() || in.size() > kBwtMaxBlock || in.size() > max_out) fail("corrupt block: BWT size out of range");
    if (primary < 1 || primary > in.size()) fail("corrupt block: BWT primary index out of range");
    const i32 n = i32(in.size());

    std::vector<u8> out(in.size());
    auto tmp = std::make_unique_for_overwrite<i32[]>(in.size() + 1);
    if (libsais_unbwt(in.data(), out.data(), tmp.get(), n, nullptr, i32(primary)) != 0)
        fail("corrupt block: inverse BWT failed");
    return out;
}

}  // namespace apx
