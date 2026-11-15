// LibFuzzer target for decompress_stream.
// Tests the full container decompressor on arbitrary/mutated input streams.
#include <cstddef>
#include <cstdint>
#include <span>

#include "container.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    apx::MemInput in(std::span<const apx::u8>(data, size));
    apx::NullOutput out;
    apx::DecompressOptions opt;
    // Fuzzing runs single-threaded to isolate issues and minimize nondeterminism.
    opt.threads = 1;
    // Soft cap decoder working memory at 64 MiB so corrupted headers claiming 1 GiB blocks
    // are rejected immediately via apx::fail() rather than causing massive allocations.
    opt.mem_limit = 64ULL * 1024ULL * 1024ULL;

    try {
        (void)apx::decompress_stream(in, out, opt);
    } catch (const apx::Error&) {
        // Expected for corrupt or truncated archives.
    }

    return 0;
}
