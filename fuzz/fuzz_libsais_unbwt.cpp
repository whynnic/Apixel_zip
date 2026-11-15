// LibFuzzer target for libsais_unbwt on arbitrary bytes and index,
// using the calling convention and buffer allocations of src/stage_bwt.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "libsais.h"

// 1 MiB cap to keep fuzzing iterations fast and responsive.
static constexpr size_t kMaxOut = 1024ULL * 1024ULL;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // The first 4 bytes supply the arbitrary 32-bit primary index.
    if (size < sizeof(int32_t)) return 0;

    int32_t primary = 0;
    std::memcpy(&primary, data, sizeof(primary));

    const uint8_t* in_data = data + sizeof(primary);
    const size_t in_size = size - sizeof(primary);
    if (in_size > kMaxOut) return 0;

    const int32_t n = static_cast<int32_t>(in_size);

    // As called in src/stage_bwt.cpp:
    // output buffer of size n, temporary index array of size n + 1.
    // Allocate at least 1 element for out so out.data() is non-null even if n == 0.
    std::vector<uint8_t> out(static_cast<size_t>(n) + 1);
    auto tmp = std::make_unique_for_overwrite<int32_t[]>(static_cast<size_t>(n) + 1);

    (void)libsais_unbwt(in_data, out.data(), tmp.get(), n, nullptr, primary);

    return 0;
}
