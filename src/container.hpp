// The .apx container: a self-describing sequence of frames.
// The byte-level specification lives in ARCHITECTURE.md ("Формат контейнера").
#pragma once

#include <cstdio>

#include "common.hpp"
#include "stages.hpp"

namespace apx {

// ---------------------------------------------------------------- I/O

struct Input {
    virtual ~Input() = default;
    // Reads up to n bytes; returns fewer than n only at end of input.
    virtual size_t read(u8* dst, size_t n) = 0;
};

struct Output {
    virtual ~Output() = default;
    virtual void write(const u8* p, size_t n) = 0;
};

class FileInput final : public Input {
public:
    explicit FileInput(std::FILE* f) : f_(f) {}
    size_t read(u8* dst, size_t n) override;

private:
    std::FILE* f_;
};

class FileOutput final : public Output {
public:
    explicit FileOutput(std::FILE* f) : f_(f) {}
    void write(const u8* p, size_t n) override;

private:
    std::FILE* f_;
};

class MemInput final : public Input {
public:
    explicit MemInput(std::span<const u8> data) : data_(data) {}
    size_t read(u8* dst, size_t n) override;

private:
    std::span<const u8> data_;
    size_t pos_ = 0;
};

class MemOutput final : public Output {
public:
    explicit MemOutput(std::vector<u8>& out) : out_(out) {}
    void write(const u8* p, size_t n) override { out_.insert(out_.end(), p, p + n); }

private:
    std::vector<u8>& out_;
};

class NullOutput final : public Output {
public:
    void write(const u8*, size_t) override {}
};

// ---------------------------------------------------------------- format constants

constexpr u8 kMagic[4] = {'A', 'P', 'X', 'L'};
constexpr u8 kFormatMajor = 0;  // readers refuse archives with a different major version
constexpr u8 kFormatMinor = 1;  // informational; bumped for backward-compatible additions
constexpr unsigned kMinBlockLog2 = 10;
constexpr unsigned kMaxBlockLog2 = 30;  // 1 GiB

constexpr u8 kFrameBlock = 0x01;
constexpr u8 kFrameEnd = 0x02;
constexpr u8 kFrameSkippableFirst = 0x80;  // 0x80..0xFF: readers skip frames they do not understand
constexpr u64 kMaxSkippableSize = u64(1) << 30;

// ---------------------------------------------------------------- options / results

struct CompressOptions {
    size_t block_size = size_t(32) << 20;  // bytes per block, 1 .. 2^30
    unsigned threads = 0;                  // 0 = all hardware threads
    u64 mem_limit = u64(4) << 30;          // soft cap on working memory; lowers parallelism, never changes output
    bool verify = true;                    // verify-on-the-fly
};

struct DecompressOptions {
    unsigned threads = 0;
    u64 mem_limit = u64(4) << 30;  // hard cap: a block that needs more is rejected
};

struct StreamStats {
    u64 raw_bytes = 0;
    u64 archive_bytes = 0;
    u64 blocks = 0;
    u64 stored_blocks = 0;
};

// The archive bytes depend only on the input and block_size — never on the
// number of threads or the memory limit.
StreamStats compress_stream(Input& in, Output& out, const CompressOptions& opt);

// Decodes and verifies an archive. Use NullOutput to test integrity only.
StreamStats decompress_stream(Input& in, Output& out, const DecompressOptions& opt);

struct BlockInfo {
    u64 raw_size = 0;
    u64 comp_size = 0;
    u64 hash = 0;
    std::vector<u8> stage_ids;
};

struct ArchiveInfo {
    u8 format_major = 0;
    u8 format_minor = 0;
    unsigned block_log2 = 0;
    u64 raw_bytes = 0;
    u64 archive_bytes = 0;
    std::vector<BlockInfo> blocks;
};

// Reads only the structure (headers are validated, payloads are skipped).
ArchiveInfo list_archive(Input& in);

}  // namespace apx
