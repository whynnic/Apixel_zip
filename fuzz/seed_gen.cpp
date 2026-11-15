// Standalone helper tool to generate valid seed files for stage decoders.
// Seeds are formatted to match the input expectations of the libFuzzer targets.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "libsais.h"
#include "stages.hpp"

namespace fs = std::filesystem;

static void write_seed(const fs::path& path, const std::vector<apx::u8>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

int main(int argc, char** argv) {
    const fs::path base_dir = (argc > 1) ? fs::path(argv[1]) : fs::path("fuzz/seeds");

    // A collection of representative inputs for initial coverage.
    std::vector<std::vector<apx::u8>> samples;

    // 1. Text sample
    {
        const std::string s = "The quick brown fox jumps over the lazy dog. Apixel v2 test seed for decoders.";
        samples.emplace_back(s.begin(), s.end());
    }

    // 2. Repetitive patterns (ideal for BWT and CM1 runs)
    {
        const std::string s = "banana banana banana abracadabra abracadabra aaaaaabbbbbbcccccc";
        samples.emplace_back(s.begin(), s.end());
    }

    // 3. Full byte range (0..255)
    {
        std::vector<apx::u8> full_range(256);
        for (size_t i = 0; i < 256; ++i) {
            full_range[i] = static_cast<apx::u8>(i);
        }
        samples.push_back(std::move(full_range));
    }

    // 4. Simulated x86 machine code containing CALL (E8) and JMP (E9) instructions
    {
        std::vector<apx::u8> code = {
            0x55, 0x48, 0x89, 0xE5,                                      // push rbp; mov rbp, rsp
            0xE8, 0x20, 0x00, 0x00, 0x00,                                // call +0x20
            0x48, 0x83, 0xC4, 0x10,                                      // add rsp, 16
            0xE9, 0x40, 0x00, 0x00, 0x00,                                // jmp +0x40
            0x5D, 0xC3,                                                  // pop rbp; ret
            0x90, 0x90, 0x90, 0x90, 0xE8, 0xFE, 0xFF, 0xFF, 0xFF, 0x90  // nops + near call
        };
        samples.push_back(std::move(code));
    }

    // 5. Simulated ARM64 machine code containing BL calls
    {
        std::vector<apx::u8> code = {
            0xFD, 0x7B, 0xBF, 0xA9,  // stp x29, x30, [sp, -16]!
            0xFD, 0x03, 0x00, 0x91,  // mov x29, sp
            0x04, 0x00, 0x00, 0x94,  // bl +16
            0x1F, 0x20, 0x03, 0xD5,  // nop
            0x02, 0x00, 0x00, 0x94,  // bl +8
            0xFD, 0x7B, 0xC1, 0xA8,  // ldp x29, x30, [sp], 16
            0xC0, 0x03, 0x5F, 0xD6,  // ret
            0x00, 0x00, 0x00, 0x14   // b +0
        };
        samples.push_back(std::move(code));
    }

    // Generate seeds for each stage decoder
    for (size_t idx = 0; idx < samples.size(); ++idx) {
        const auto& in = samples[idx];
        const std::string id = std::to_string(idx);

        // --- BWT ---
        {
            std::vector<apx::u8> params;
            const std::vector<apx::u8> payload = apx::bwt_encode(in, params);
            std::vector<apx::u8> seed;
            seed.reserve(1 + params.size() + payload.size());
            seed.push_back(static_cast<apx::u8>(params.size()));
            seed.insert(seed.end(), params.begin(), params.end());
            seed.insert(seed.end(), payload.begin(), payload.end());
            write_seed(base_dir / "fuzz_bwt_decode" / ("seed_" + id), seed);
        }

        // --- CM1 ---
        {
            std::vector<apx::u8> params;
            const std::vector<apx::u8> payload = apx::cm1_encode(in, params);
            std::vector<apx::u8> seed;
            seed.reserve(1 + params.size() + payload.size());
            seed.push_back(static_cast<apx::u8>(params.size()));
            seed.insert(seed.end(), params.begin(), params.end());
            seed.insert(seed.end(), payload.begin(), payload.end());
            write_seed(base_dir / "fuzz_cm1_decode" / ("seed_" + id), seed);
        }

        // --- X86 ---
        {
            std::vector<apx::u8> params;
            const std::vector<apx::u8> payload = apx::x86_encode(in, params);
            std::vector<apx::u8> seed;
            seed.reserve(1 + payload.size());
            seed.push_back(0);  // X86 filter expects 0 parameter bytes
            seed.insert(seed.end(), payload.begin(), payload.end());
            write_seed(base_dir / "fuzz_x86_decode" / ("seed_" + id), seed);
        }

        // --- ARM64 ---
        {
            std::vector<apx::u8> params;
            const std::vector<apx::u8> payload = apx::arm64_encode(in, params);
            std::vector<apx::u8> seed;
            seed.reserve(1 + payload.size());
            seed.push_back(0);  // ARM64 filter expects 0 parameter bytes
            seed.insert(seed.end(), payload.begin(), payload.end());
            write_seed(base_dir / "fuzz_arm64_decode" / ("seed_" + id), seed);
        }

        // --- libsais_unbwt ---
        {
            const int32_t n = static_cast<int32_t>(in.size());
            std::vector<apx::u8> bwt_out(in.size());
            auto sa = std::make_unique_for_overwrite<int32_t[]>(in.size());
            const int32_t primary = libsais_bwt(in.data(), bwt_out.data(), sa.get(), n, 0, nullptr);
            if (primary > 0) {
                std::vector<apx::u8> seed;
                seed.resize(sizeof(int32_t) + bwt_out.size());
                std::memcpy(seed.data(), &primary, sizeof(primary));
                std::memcpy(seed.data() + sizeof(primary), bwt_out.data(), bwt_out.size());
                write_seed(base_dir / "fuzz_libsais_unbwt" / ("seed_" + id), seed);
            }
        }
    }

    return 0;
}
