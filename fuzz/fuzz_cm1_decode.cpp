// LibFuzzer target for stage CM1 decoder (cm1_decode).
// First byte gives the length of the stage parameters; remainder is the input data.
#include <cstddef>
#include <cstdint>
#include <span>

#include "stages.hpp"

// 2 KiB cap on maximum output size to keep fuzzing throughput high (>= 500 exec/s).
// Without this cap, corrupted headers specifying large sizes (up to 1 MiB) cause the decoder
// to iterate through the adaptive context mixer (~35 ms per run at 1 MiB), dragging fuzzing
// throughput down to ~28 exec/s. At 2 KiB, throughput stays comfortably >= 500 exec/s even
// under concurrent execution, while thoroughly exercising all CM1 prediction and arithmetic paths.
// fuzz_cm1_decode_big is the same source built with APX_FUZZ_MAX_OUT=1 MiB: slow, but it reaches states that
// only long streams hit (counter saturation, table wrap-around); run it on milestones.
#ifndef APX_FUZZ_MAX_OUT
#define APX_FUZZ_MAX_OUT 2048
#endif
static constexpr size_t kMaxOut = APX_FUZZ_MAX_OUT;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    // The first byte determines how many subsequent bytes represent stage parameters.
    const size_t param_len = static_cast<size_t>(data[0]);
    if (param_len > size - 1) return 0;

    std::span<const apx::u8> params(data + 1, param_len);
    std::span<const apx::u8> in(data + 1 + param_len, size - 1 - param_len);

    try {
        (void)apx::cm1_decode(params, in, kMaxOut);
    } catch (const apx::Error&) {
        // Expected on invalid parameters or corrupt CM1 bitstream.
    }

    return 0;
}
