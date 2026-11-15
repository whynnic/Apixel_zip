// apixel: command-line front end.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "container.hpp"

namespace {

using namespace apx;

constexpr const char* kVersion = "2.0.0-dev";

void usage(std::FILE* f) {
    std::fputs(
        "apixel " /* version printed separately */
        "- experimental lossless compressor\n"
        "\n"
        "Usage:\n"
        "  apixel c [options] <input>    compress   (default output: <input>.apx)\n"
        "  apixel x [options] <archive>  extract    (default output: <archive> without .apx)\n"
        "  apixel t [options] <archive>  test integrity (decodes everything, writes nothing)\n"
        "  apixel l <archive>            list blocks\n"
        "\n"
        "Options:\n"
        "  -o <file>     output file; '-' means stdout\n"
        "  -b <MiB>      block size in MiB, 1..1024 (default 32); larger = better ratio, more memory\n"
        "  -T <n>        threads (default: all hardware threads)\n"
        "  -M <MiB>      memory limit (default 4096)\n"
        "  -f            overwrite an existing output file\n"
        "  -q            quiet\n"
        "  --no-verify   skip verify-on-the-fly when compressing (faster, less safe)\n"
        "  -h, --help    this text\n"
        "  --version     print version\n"
        "\n"
        "Use '-' as <input>/<archive> to read from stdin.\n",
        f);
}

struct Args {
    std::string cmd;
    std::string input;
    std::string output;
    bool has_output = false;
    bool force = false;
    bool quiet = false;
    bool verify = true;
    unsigned long block_mib = 32;
    unsigned long threads = 0;
    unsigned long mem_mib = 4096;
};

unsigned long parse_number(const char* s, const char* what, unsigned long lo, unsigned long hi) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi)
        fail(std::string("invalid value for ") + what + ": '" + s + "'");
    return v;
}

Args parse_args(int argc, char** argv) {
    Args a;
    a.cmd = argv[1];
    bool have_input = false;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto value = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) fail(std::string("option ") + opt + " needs a value");
            return argv[++i];
        };
        if (s == "-o") {
            a.output = value("-o");
            a.has_output = true;
        } else if (s == "-b") {
            a.block_mib = parse_number(value("-b"), "-b", 1, 1024);
        } else if (s == "-T") {
            a.threads = parse_number(value("-T"), "-T", 1, 1024);
        } else if (s == "-M") {
            a.mem_mib = parse_number(value("-M"), "-M", 64, 1ul << 24);
        } else if (s == "-f") {
            a.force = true;
        } else if (s == "-q") {
            a.quiet = true;
        } else if (s == "--no-verify") {
            a.verify = false;
        } else if (s != "-" && !s.empty() && s[0] == '-') {
            fail("unknown option '" + s + "'");
        } else {
            if (have_input) fail("more than one input given");
            a.input = s;
            have_input = true;
        }
    }
    if (!have_input) fail("no input given");
    return a;
}

bool file_exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// RAII for the input stream.
struct InFile {
    std::FILE* f = nullptr;
    bool owned = false;
    explicit InFile(const std::string& path) {
        if (path == "-") {
            f = stdin;
        } else {
            f = std::fopen(path.c_str(), "rb");
            if (!f) fail("cannot open '" + path + "' for reading");
            owned = true;
        }
    }
    ~InFile() {
        if (owned && f) std::fclose(f);
    }
    InFile(const InFile&) = delete;
    InFile& operator=(const InFile&) = delete;
};

// Output goes to "<path>.tmp" and is renamed on commit(), so a failed run
// never leaves a half-written file under the final name.
struct OutFile {
    std::FILE* f = nullptr;
    std::string path, tmp;
    bool to_stdout = false;
    bool committed = false;

    OutFile(const std::string& p, bool force) : path(p) {
        if (p == "-") {
            if (::isatty(STDOUT_FILENO) && !force) fail("refusing to write binary data to a terminal (use -o or -f)");
            f = stdout;
            to_stdout = true;
            return;
        }
        if (!force && file_exists(p)) fail("output '" + p + "' already exists (use -f to overwrite)");
        tmp = p + ".tmp";
        f = std::fopen(tmp.c_str(), "wb");
        if (!f) fail("cannot open '" + tmp + "' for writing");
    }

    void commit() {
        if (to_stdout) {
            if (std::fflush(f) != 0) fail("write error (disk full?)");
        } else {
            const bool ok = std::fclose(f) == 0;
            f = nullptr;
            if (!ok) fail("write error while closing '" + tmp + "'");
            if (std::rename(tmp.c_str(), path.c_str()) != 0) fail("cannot rename '" + tmp + "' to '" + path + "'");
        }
        committed = true;
    }

    ~OutFile() {
        if (to_stdout || committed) return;
        if (f) std::fclose(f);
        std::remove(tmp.c_str());
    }
    OutFile(const OutFile&) = delete;
    OutFile& operator=(const OutFile&) = delete;
};

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// `packing` selects the direction shown: raw -> archive, or archive -> raw.
void report(const char* verb, const StreamStats& st, double sec, bool packing) {
    const double mib = double(st.raw_bytes) / (1024.0 * 1024.0);
    const double ratio = st.raw_bytes ? 100.0 * double(st.archive_bytes) / double(st.raw_bytes) : 0.0;
    const unsigned long long from = packing ? st.raw_bytes : st.archive_bytes;
    const unsigned long long to = packing ? st.archive_bytes : st.raw_bytes;
    std::fprintf(stderr, "%s: %llu -> %llu bytes (archive is %.2f%% of original), %llu block(s), %llu stored, %.2f s, %.1f MiB/s\n",
                 verb, from, to, ratio, static_cast<unsigned long long>(st.blocks),
                 static_cast<unsigned long long>(st.stored_blocks), sec, sec > 0 ? mib / sec : 0.0);
}

int cmd_compress(const Args& a) {
    std::string out_path = a.has_output ? a.output : (a.input == "-" ? std::string("-") : a.input + ".apx");
    InFile in(a.input);
    OutFile out(out_path, a.force);

    CompressOptions opt;
    opt.block_size = size_t(a.block_mib) << 20;
    opt.threads = unsigned(a.threads);
    opt.mem_limit = u64(a.mem_mib) << 20;
    opt.verify = a.verify;

    FileInput fin(in.f);
    FileOutput fout(out.f);
    const auto t0 = std::chrono::steady_clock::now();
    const StreamStats st = compress_stream(fin, fout, opt);
    out.commit();
    if (!a.quiet) report("compressed", st, seconds_since(t0), true);
    return 0;
}

int cmd_extract(const Args& a, bool test_only) {
    InFile in(a.input);
    DecompressOptions opt;
    opt.threads = unsigned(a.threads);
    opt.mem_limit = u64(a.mem_mib) << 20;
    FileInput fin(in.f);
    const auto t0 = std::chrono::steady_clock::now();

    if (test_only) {
        NullOutput sink;
        const StreamStats st = decompress_stream(fin, sink, opt);
        if (!a.quiet) {
            report("tested", st, seconds_since(t0), false);
            std::fprintf(stderr, "OK\n");
        }
        return 0;
    }

    std::string out_path;
    if (a.has_output) {
        out_path = a.output;
    } else if (a.input == "-") {
        out_path = "-";
    } else if (ends_with(a.input, ".apx")) {
        out_path = a.input.substr(0, a.input.size() - 4);
    } else {
        fail("cannot guess the output name (archive does not end in .apx); use -o");
    }

    OutFile out(out_path, a.force);
    FileOutput fout(out.f);
    const StreamStats st = decompress_stream(fin, fout, opt);
    out.commit();
    if (!a.quiet) report("extracted", st, seconds_since(t0), false);
    return 0;
}

int cmd_list(const Args& a) {
    InFile in(a.input);
    FileInput fin(in.f);
    const ArchiveInfo info = list_archive(fin);
    std::printf("format %u.%u, max block %llu bytes, %zu block(s)\n", info.format_major, info.format_minor,
                1ull << info.block_log2, info.blocks.size());
    std::printf("%6s %12s %12s %8s  %-16s %s\n", "block", "raw", "packed", "ratio", "xxh64", "stages");
    size_t i = 0;
    for (const BlockInfo& b : info.blocks) {
        std::string chain;
        if (b.stage_ids.empty()) chain = "stored";
        for (const u8 id : b.stage_ids) {
            const char* name = stage_name(id);
            if (!chain.empty()) chain += " > ";
            chain += name ? name : ("?" + std::to_string(id));
        }
        std::printf("%6zu %12llu %12llu %7.2f%%  %016llx %s\n", i++, static_cast<unsigned long long>(b.raw_size),
                    static_cast<unsigned long long>(b.comp_size), 100.0 * double(b.comp_size) / double(b.raw_size),
                    static_cast<unsigned long long>(b.hash), chain.c_str());
    }
    std::printf("total: %llu -> %llu bytes (%.2f%%)\n", static_cast<unsigned long long>(info.raw_bytes),
                static_cast<unsigned long long>(info.archive_bytes),
                info.raw_bytes ? 100.0 * double(info.archive_bytes) / double(info.raw_bytes) : 0.0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage(stdout);
        return 0;
    }
    if (cmd == "--version" || cmd == "version") {
        std::printf("apixel %s (container format %u.%u)\n", kVersion, apx::kFormatMajor, apx::kFormatMinor);
        return 0;
    }
    try {
        if (cmd != "c" && cmd != "x" && cmd != "t" && cmd != "l") {
            std::fprintf(stderr, "apixel: unknown command '%s'\n\n", cmd.c_str());
            usage(stderr);
            return 2;
        }
        const Args a = parse_args(argc, argv);
        if (cmd == "c") return cmd_compress(a);
        if (cmd == "x") return cmd_extract(a, false);
        if (cmd == "t") return cmd_extract(a, true);
        return cmd_list(a);
    } catch (const apx::Error& e) {
        std::fprintf(stderr, "apixel: error: %s\n", e.what());
        return 1;
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "apixel: error: out of memory (try a smaller -b or fewer threads with -T)\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "apixel: error: %s\n", e.what());
        return 1;
    }
}
