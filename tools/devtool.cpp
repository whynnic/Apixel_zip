// Developer tool for model experiments. Not part of the product.
//
//   devtool bwt <in> <out>     write the BWT of <in> (one block) to <out>
//   devtool cm1 <file>...      code each file with CM1 as-is (files are expected
//                              to be BWT output) and print sizes
//
// Built with -DAPX_TUNE, the CM1 constants can be overridden from the
// environment (e.g. kLimL1=16 devtool cm1 ...) and a cost breakdown per
// decision type is printed. See APX_PARAM in src/stage_cm1.cpp.
#include <chrono>
#include <cstdio>
#include <string>

#include "stages.hpp"

#ifdef APX_TUNE
namespace apx::tune {
extern double cost[3];
extern unsigned long long count[3];
}  // namespace apx::tune
#endif

using namespace apx;

static std::vector<u8> read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) fail(std::string("cannot open ") + path);
    std::vector<u8> v;
    u8 buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    return v;
}

int main(int argc, char** argv) {
    try {
        const std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "bwt" && argc == 4) {
            const std::vector<u8> in = read_file(argv[2]);
            std::vector<u8> params;
            const std::vector<u8> out = bwt_encode(in, params);
            std::FILE* f = std::fopen(argv[3], "wb");
            if (!f || std::fwrite(out.data(), 1, out.size(), f) != out.size()) fail("write failed");
            std::fclose(f);
            return 0;
        }
        if (cmd == "cm1" && argc >= 3) {
            unsigned long long total_in = 0, total_out = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 2; i < argc; ++i) {
                const std::vector<u8> in = read_file(argv[i]);
                std::vector<u8> params;
                const std::vector<u8> out = cm1_encode(in, params);
                total_in += in.size();
                total_out += out.size();
                const char* base = argv[i];
                for (const char* p = argv[i]; *p; ++p)
                    if (*p == '/') base = p + 1;
                std::printf("%-14s %9zu\n", base, out.size());
            }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("TOTAL %llu -> %llu  (%.2f MiB/s)\n", total_in, total_out,
                        double(total_in) / 1048576.0 / sec);
#ifdef APX_TUNE
            const char* names[3] = {"A (run)", "B (rank1)", "L (literal bits)"};
            for (int k = 0; k < 3; ++k)
                std::printf("  %-18s %12llu decisions %12.0f bytes\n", names[k], tune::count[k], tune::cost[k] / 8);
#endif
            return 0;
        }
        std::fprintf(stderr, "usage: devtool bwt <in> <out> | devtool cm1 <file>...\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "devtool: %s\n", e.what());
        return 1;
    }
}
