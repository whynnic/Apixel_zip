// LibFuzzer target for stage BWT decoder (bwt_decode).
// First byte gives the length of the stage parameters; remainder is the input data.
#include <cstddef>
#include <cstdint>
#include <span>

#include "stages.hpp"

// 1 MiB cap on maximum output size to prevent unbounded allocations during fuzzing.
static constexpr size_t kMaxOut = 1024ULL * 1024ULL;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    // The first byte determines how many subsequent bytes represent stage parameters.
    const size_t param_len = static_cast<size_t>(data[0]);
    if (param_len > size - 1) return 0;

    std::span<const apx::u8> params(data + 1, param_len);
    std::span<const apx::u8> in(data + 1 + param_len, size - 1 - param_len);

    try {
        (void)apx::bwt_decode(params, in, kMaxOut);
    } catch (const apx::Error&) {
        // Expected on invalid parameters or corrupt BWT payload.
    }

    return 0;
}
