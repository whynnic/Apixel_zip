// .apx container: writing, reading, and the parallel block pipeline.
//
// Threading model: blocks are independent. A bounded pipeline reads blocks,
// encodes or decodes them across a worker pool, and writes the results strictly
// in block index order. The number of threads and the memory limit only affect
// speed and memory usage, never the archive bytes produced.
#include "container.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xxh64.hpp"

namespace apx {

// ---------------------------------------------------------------- I/O

size_t FileInput::read(u8* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const size_t r = std::fread(dst + got, 1, n - got, f_);
        if (r == 0) {
            if (std::ferror(f_)) fail("read error");
            break;
        }
        got += r;
    }
    return got;
}

void FileOutput::write(const u8* p, size_t n) {
    if (n != 0 && std::fwrite(p, 1, n, f_) != n) fail("write error (disk full?)");
}

size_t MemInput::read(u8* dst, size_t n) {
    const size_t k = std::min(n, data_.size() - pos_);
    if (k != 0) std::memcpy(dst, data_.data() + pos_, k);
    pos_ += k;
    return k;
}

namespace {

unsigned effective_threads(unsigned requested) {
    if (requested != 0) return requested;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw != 0 ? hw : 1;
}

// ---------------------------------------------------------------- frame writers

// Every header ends with the low 32 bits of XXH64 over the header bytes before it.
void append_check(std::vector<u8>& h) { put_u32le(h, u32(xxh64(h))); }

std::vector<u8> make_file_header(unsigned block_log2) {
    std::vector<u8> h(kMagic, kMagic + 4);
    put_u8(h, kFormatMajor);
    put_u8(h, kFormatMinor);
    put_u8(h, u8(block_log2));
    put_u8(h, 0);  // flags, must be 0 in this version
    append_check(h);
    return h;
}

std::vector<u8> make_block_header(u64 raw_size, u64 comp_size, const std::vector<StageDesc>& stages, u64 hash) {
    std::vector<u8> h;
    put_u8(h, kFrameBlock);
    put_varint(h, raw_size);
    put_varint(h, comp_size);
    put_u8(h, u8(stages.size()));
    for (const StageDesc& s : stages) {
        if (s.params.size() > kMaxStageParams) fail("internal error: stage parameters too long");
        put_u8(h, s.id);
        put_u8(h, u8(s.params.size()));
        h.insert(h.end(), s.params.begin(), s.params.end());
    }
    put_u64le(h, hash);
    append_check(h);
    return h;
}

std::vector<u8> make_end_frame(u64 total_raw, u64 blocks, const std::vector<u8>& hash_list) {
    std::vector<u8> h;
    put_u8(h, kFrameEnd);
    put_varint(h, total_raw);
    put_varint(h, blocks);
    put_u64le(h, xxh64(hash_list));
    append_check(h);
    return h;
}

// ---------------------------------------------------------------- frame reader

// Sequential reader that records header bytes so their checksum can be verified.
class FrameParser {
public:
    explicit FrameParser(Input& in) : in_(in) {}

    void begin() { rec_.clear(); }

    bool try_get_u8(u8& v) {
        if (in_.read(&v, 1) != 1) return false;
        rec_.push_back(v);
        ++consumed_;
        return true;
    }

    u8 get_u8() {
        u8 v;
        if (!try_get_u8(v)) fail("truncated archive");
        return v;
    }

    u64 get_varint() {
        u64 v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const u8 b = get_u8();
            if (shift == 63 && (b & 0xFE) != 0) fail("corrupt archive: varint overflows 64 bits");
            v |= u64(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        fail("corrupt archive: varint too long");
    }

    u64 get_u64le() {
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= u64(get_u8()) << (8 * i);
        return v;
    }

    void get_bytes(std::vector<u8>& dst, size_t n) {
        dst.resize(n);
        for (size_t i = 0; i < n; ++i) dst[i] = get_u8();
    }

    // Reads the 4-byte header check and compares it with the recorded bytes.
    void check() {
        u8 b[4];
        read_raw(b, 4);
        u32 stored;
        std::memcpy(&stored, b, 4);
        if (stored != u32(xxh64(rec_))) fail("corrupt archive: header checksum mismatch");
    }

    void read_raw(u8* dst, size_t n) {
        if (in_.read(dst, n) != n) fail("truncated archive");
        consumed_ += n;
    }

    void skip(u64 n) {
        std::vector<u8> scratch(size_t(std::min<u64>(n, 1u << 16)));
        while (n != 0) {
            const size_t k = size_t(std::min<u64>(n, scratch.size()));
            read_raw(scratch.data(), k);
            n -= k;
        }
    }

    u64 consumed() const { return consumed_; }

private:
    Input& in_;
    std::vector<u8> rec_;
    u64 consumed_ = 0;
};

struct BlockHeader {
    u64 raw_size = 0;
    u64 comp_size = 0;
    u64 hash = 0;
    std::vector<StageDesc> stages;
};

// Walks the frames of an archive, validating structure as it goes.
// Usage: while (next_block(h)) { read_payload(...) or skip_payload(...); }
class ArchiveReader {
public:
    explicit ArchiveReader(Input& in) : p_(in) {
        u8 h[12];
        if (in.read(h, 12) != 12) fail("not an apixel archive (too short)");
        if (std::memcmp(h, kMagic, 4) != 0) fail("not an apixel archive (bad magic)");
        u32 stored;
        std::memcpy(&stored, h + 8, 4);
        if (stored != u32(xxh64(h, 8))) fail("corrupt archive: file header checksum mismatch");
        format_major = h[4];
        format_minor = h[5];
        block_log2 = h[6];
        if (format_major != kFormatMajor)
            fail("unsupported archive format version " + std::to_string(format_major) + "." +
                 std::to_string(format_minor));
        if (h[7] != 0) fail("unsupported archive flags (written by a newer apixel?)");
        if (block_log2 < kMinBlockLog2 || block_log2 > kMaxBlockLog2)
            fail("corrupt archive: block size out of range");
        max_block_ = u64(1) << block_log2;
    }

    // Parses frames up to the next block header. Returns false once the end
    // frame has been read and matched against everything seen before it.
    bool next_block(BlockHeader& h) {
        for (;;) {
            p_.begin();
            u8 type;
            if (!p_.try_get_u8(type)) fail("truncated archive: the end frame is missing");

            if (type == kFrameBlock) {
                h = BlockHeader{};
                h.raw_size = p_.get_varint();
                h.comp_size = p_.get_varint();
                const u8 nstages = p_.get_u8();
                // Range checks first: nothing below may allocate based on unchecked sizes.
                if (h.raw_size == 0 || h.raw_size > max_block_) fail("corrupt archive: block size out of range");
                // comp_size may be 0: a coder stream consisting only of zeros is trimmed to nothing.
                if (h.comp_size > h.raw_size) fail("corrupt archive: compressed size out of range");
                if (nstages > kMaxStages) fail("corrupt archive: too many stages");
                if (nstages == 0 && h.comp_size != h.raw_size) fail("corrupt archive: bad stored block");
                h.stages.resize(nstages);
                for (StageDesc& s : h.stages) {
                    s.id = p_.get_u8();
                    const u8 len = p_.get_u8();
                    p_.get_bytes(s.params, len);
                }
                h.hash = p_.get_u64le();
                p_.check();
                ++blocks_;
                total_raw_ += h.raw_size;
                put_u64le(hash_list_, h.hash);
                return true;
            }

            if (type == kFrameEnd) {
                const u64 total = p_.get_varint();
                const u64 count = p_.get_varint();
                const u64 stream_hash = p_.get_u64le();
                p_.check();
                if (total != total_raw_ || count != blocks_ || stream_hash != xxh64(hash_list_))
                    fail("corrupt archive: end frame does not match the blocks (missing or reordered data)");
                u8 extra;
                if (p_.try_get_u8(extra)) fail("corrupt archive: unexpected data after the end frame");
                return false;
            }

            if (type >= kFrameSkippableFirst) {  // extension frame from a newer version: skip it
                const u64 len = p_.get_varint();
                p_.check();
                if (len > kMaxSkippableSize) fail("corrupt archive: extension frame too large");
                p_.skip(len);
                continue;
            }

            fail("corrupt archive: unknown frame type " + std::to_string(type));
        }
    }

    void read_payload(std::vector<u8>& dst, u64 comp_size) {
        dst.resize(size_t(comp_size));
        p_.read_raw(dst.data(), dst.size());
    }

    void skip_payload(u64 comp_size) { p_.skip(comp_size); }

    u64 consumed() const { return 12 + p_.consumed(); }

    u8 format_major = 0;
    u8 format_minor = 0;
    unsigned block_log2 = 0;

private:
    FrameParser p_;
    u64 max_block_ = 0;
    u64 blocks_ = 0;
    u64 total_raw_ = 0;
    std::vector<u8> hash_list_;
};

// ---------------------------------------------------------------- pipeline

// Shared state between the coordinator thread (which reads input and writes
// ordered output) and the pool of worker threads.
//
// Memory accounting invariant:
// Every block that has been read from Input and has not yet finished being
// written to Output contributes its `mem_cost` to `in_flight_mem` and 1 to
// `in_flight_blocks`. A new block may be admitted when:
//   in_flight_blocks == 0 || (in_flight_mem + cost <= mem_limit && in_flight_blocks < max_in_flight)
// This guarantees that:
//   1. At least one block can always make progress (even if mem_limit < cost
//      in compression where mem_limit is a soft cap).
//   2. Total estimated memory of all blocks in flight (waiting in `pending`,
//      currently being encoded/decoded by workers, or waiting in `ready` for
//      earlier blocks to finish) never exceeds `mem_limit` when >1 block is
//      in flight.
template <class Task>
struct PipelineState {
    std::mutex mu;
    std::condition_variable cv_worker;
    std::condition_variable cv_coord;

    std::deque<std::unique_ptr<Task>> pending;
    std::vector<std::unique_ptr<Task>> ready;

    u64 in_flight_mem = 0;
    size_t in_flight_blocks = 0;
    bool input_done = false;
    bool aborted = false;
    std::exception_ptr first_error;

    void record_error(std::exception_ptr ep) {
        std::lock_guard<std::mutex> lock(mu);
        if (!first_error) first_error = ep;
        aborted = true;
        cv_worker.notify_all();
        cv_coord.notify_all();
    }

    void abort_now() {
        std::lock_guard<std::mutex> lock(mu);
        aborted = true;
        cv_worker.notify_all();
        cv_coord.notify_all();
    }

    // Extracts the ready task with `index == expected_idx` if present.
    // Must be called with `mu` locked.
    std::unique_ptr<Task> take_ready_locked(u64 expected_idx) {
        for (size_t i = 0; i < ready.size(); ++i) {
            if (ready[i]->index == expected_idx) {
                std::unique_ptr<Task> t = std::move(ready[i]);
                ready[i] = std::move(ready.back());
                ready.pop_back();
                return t;
            }
        }
        return nullptr;
    }
};

// RAII guard that ensures all worker threads are joined on any exit path
// (including exceptions thrown by the reader or writer on the main thread).
template <class Task>
class WorkerPool {
public:
    template <class WorkFn>
    WorkerPool(PipelineState<Task>& state, unsigned threads, WorkFn work_fn) : state_(state) {
        threads_.reserve(threads);
        try {
            for (unsigned t = 0; t < threads; ++t) {
                threads_.emplace_back([&state, work_fn]() {
                    for (;;) {
                        std::unique_ptr<Task> task;
                        {
                            std::unique_lock<std::mutex> lock(state.mu);
                            state.cv_worker.wait(lock, [&] {
                                return state.aborted || !state.pending.empty() || state.input_done;
                            });
                            if (state.aborted) return;
                            if (state.pending.empty()) return;  // input_done && pending.empty()
                            task = std::move(state.pending.front());
                            state.pending.pop_front();
                        }
                        try {
                            work_fn(*task);
                        } catch (...) {
                            state.record_error(std::current_exception());
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(state.mu);
                            if (state.aborted) return;
                            state.ready.push_back(std::move(task));
                            state.cv_coord.notify_one();
                        }
                    }
                });
            }
        } catch (...) {
            state_.record_error(std::current_exception());
            join_all();
            throw;
        }
    }

    ~WorkerPool() {
        state_.abort_now();
        join_all();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void join_all() {
        for (std::thread& th : threads_) {
            if (th.joinable()) th.join();
        }
    }

private:
    PipelineState<Task>& state_;
    std::vector<std::thread> threads_;
};

}  // namespace

// ---------------------------------------------------------------- compress

StreamStats compress_stream(Input& in, Output& out, const CompressOptions& opt) {
    if (opt.block_size == 0 || opt.block_size > (size_t(1) << kMaxBlockLog2))
        fail("block size must be between 1 byte and 1 GiB");
    unsigned block_log2 = kMinBlockLog2;
    while ((size_t(1) << block_log2) < opt.block_size) ++block_log2;

    const CodecOptions codec{opt.verify};
    const unsigned threads = effective_threads(opt.threads);

    StreamStats st;
    std::vector<u8> hash_list;
    auto emit = [&](const std::vector<u8>& bytes) {
        out.write(bytes.data(), bytes.size());
        st.archive_bytes += bytes.size();
    };
    emit(make_file_header(block_log2));

    // Fast sequential path for -T 1: no extra threads or synchronization overhead.
    if (threads <= 1) {
        std::unique_ptr<u8[]> raw = std::make_unique_for_overwrite<u8[]>(opt.block_size);
        for (;;) {
            const size_t n = in.read(raw.get(), opt.block_size);
            if (n == 0) break;
            const std::span<const u8> span(raw.get(), n);
            const u64 hash = xxh64(span);
            const EncodedBlock enc = encode_block(span, codec);
            const bool stored = enc.stored();
            const size_t comp = stored ? n : enc.payload.size();
            emit(make_block_header(n, comp, enc.stages, hash));
            out.write(stored ? raw.get() : enc.payload.data(), comp);
            st.archive_bytes += comp;
            st.raw_bytes += n;
            st.blocks += 1;
            st.stored_blocks += stored ? 1 : 0;
            put_u64le(hash_list, hash);
            if (n < opt.block_size) break;
        }
        emit(make_end_frame(st.raw_bytes, st.blocks, hash_list));
        return st;
    }

    struct CompressTask {
        u64 index = 0;
        u64 mem_cost = 0;
        std::unique_ptr<u8[]> raw;
        size_t size = 0;
        u64 hash = 0;
        EncodedBlock enc;
    };

    // Cap the number of in-flight blocks to bound queue depth while allowing
    // enough slack for reading and writing to overlap with worker execution.
    const size_t max_in_flight = size_t(threads) * 2;
    const u64 single_block_est = block_memory_estimate(opt.block_size);

    PipelineState<CompressTask> state;
    WorkerPool<CompressTask> pool(state, threads, [codec](CompressTask& t) {
        const std::span<const u8> raw(t.raw.get(), t.size);
        t.hash = xxh64(raw);
        t.enc = encode_block(raw, codec);
        if (!t.enc.stored()) {
            // Raw buffer is no longer needed once encoded and verified.
            t.raw.reset();
        }
    });

    u64 next_read = 0;
    u64 next_write = 0;
    bool eof = false;

    auto write_task = [&](CompressTask& t) {
        const bool stored = t.enc.stored();
        const size_t comp = stored ? t.size : t.enc.payload.size();
        emit(make_block_header(t.size, comp, t.enc.stages, t.hash));
        out.write(stored ? t.raw.get() : t.enc.payload.data(), comp);
        st.archive_bytes += comp;
        st.raw_bytes += t.size;
        st.blocks += 1;
        st.stored_blocks += stored ? 1 : 0;
        put_u64le(hash_list, t.hash);
        // Free buffers before releasing the memory budget.
        t.raw.reset();
        t.enc.payload = {};
        t.enc.stages = {};
    };

    try {
        while (!eof || next_write < next_read) {
            // 1. Drain any contiguous ready blocks first to free memory and keep Output busy.
            for (;;) {
                std::unique_ptr<CompressTask> done;
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    if (state.aborted) break;
                    done = state.take_ready_locked(next_write);
                }
                if (!done) break;
                const u64 cost = done->mem_cost;
                write_task(*done);
                ++next_write;
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.in_flight_mem -= cost;
                    --state.in_flight_blocks;
                }
            }

            {
                std::lock_guard<std::mutex> lock(state.mu);
                if (state.aborted) break;
            }

            // 2. Read and enqueue new blocks while within memory and concurrency limits.
            bool read_any = false;
            while (!eof) {
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    if (state.aborted) break;
                    // Check if the next block can be admitted without blocking.
                    const bool can_admit =
                        state.in_flight_blocks == 0 ||
                        (state.in_flight_blocks < max_in_flight &&
                         state.in_flight_mem + single_block_est <= opt.mem_limit);
                    if (!can_admit) break;
                }

                auto task = std::make_unique<CompressTask>();
                task->raw = std::make_unique_for_overwrite<u8[]>(opt.block_size);
                task->size = in.read(task->raw.get(), opt.block_size);
                if (task->size < opt.block_size) eof = true;
                if (task->size == 0) {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.input_done = true;
                    state.cv_worker.notify_all();
                    break;
                }

                task->index = next_read++;
                task->mem_cost = block_memory_estimate(task->size);
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.in_flight_mem += task->mem_cost;
                    ++state.in_flight_blocks;
                    state.pending.push_back(std::move(task));
                    if (eof) state.input_done = true;
                }
                if (eof) {
                    state.cv_worker.notify_all();
                } else {
                    state.cv_worker.notify_one();
                }
                read_any = true;
            }

            // 3. If no more blocks can be read right now and we still have blocks
            // in flight, wait until the next sequential block is ready to write.
            if (!read_any && next_write < next_read) {
                std::unique_ptr<CompressTask> done;
                {
                    std::unique_lock<std::mutex> lock(state.mu);
                    state.cv_coord.wait(lock, [&] {
                        if (state.aborted) return true;
                        done = state.take_ready_locked(next_write);
                        return done != nullptr;
                    });
                    if (state.aborted) break;
                }
                if (done) {
                    const u64 cost = done->mem_cost;
                    write_task(*done);
                    ++next_write;
                    {
                        std::lock_guard<std::mutex> lock(state.mu);
                        state.in_flight_mem -= cost;
                        --state.in_flight_blocks;
                    }
                }
            }
        }
    } catch (...) {
        state.record_error(std::current_exception());
    }

    {
        std::lock_guard<std::mutex> lock(state.mu);
        state.input_done = true;
        state.cv_worker.notify_all();
    }
    pool.join_all();

    if (state.first_error) std::rethrow_exception(state.first_error);

    emit(make_end_frame(st.raw_bytes, st.blocks, hash_list));
    return st;
}

// ---------------------------------------------------------------- decompress

StreamStats decompress_stream(Input& in, Output& out, const DecompressOptions& opt) {
    ArchiveReader ar(in);
    const unsigned threads = effective_threads(opt.threads);
    StreamStats st;

    // Fast sequential path for -T 1: no extra threads or synchronization overhead.
    if (threads <= 1) {
        u64 index = 0;
        BlockHeader h;
        std::vector<u8> payload;
        while (ar.next_block(h)) {
            const u64 need = h.stages.empty() ? h.raw_size : block_memory_estimate(h.raw_size);
            if (need > opt.mem_limit)
                fail("block " + std::to_string(index) + " needs about " + std::to_string(need >> 20) +
                     " MiB to decode, above the memory limit (raise it with -M)");
            ar.read_payload(payload, h.comp_size);
            std::vector<u8> decoded;
            if (h.stages.empty()) {
                decoded = std::move(payload);
            } else {
                decoded = decode_block(h.stages, payload, size_t(h.raw_size));
                payload = {};
            }
            if (xxh64(decoded) != h.hash)
                fail("corrupt archive: checksum mismatch in block " + std::to_string(index));
            out.write(decoded.data(), decoded.size());
            st.raw_bytes += decoded.size();
            st.blocks += 1;
            st.stored_blocks += h.stages.empty() ? 1 : 0;
            ++index;
        }
        st.archive_bytes = ar.consumed();
        return st;
    }

    struct DecompressTask {
        u64 index = 0;
        u64 mem_cost = 0;
        BlockHeader h;
        std::vector<u8> payload;
        std::vector<u8> out;
    };

    const size_t max_in_flight = size_t(threads) * 2;

    PipelineState<DecompressTask> state;
    WorkerPool<DecompressTask> pool(state, threads, [](DecompressTask& t) {
        if (t.h.stages.empty()) {
            t.out = std::move(t.payload);
        } else {
            t.out = decode_block(t.h.stages, t.payload, size_t(t.h.raw_size));
            t.payload = {};
        }
        // Validate the block hash before the task can ever reach the writer.
        // This guarantees nothing from a corrupted block is written to Output.
        if (xxh64(t.out) != t.h.hash)
            fail("corrupt archive: checksum mismatch in block " + std::to_string(t.index));
    });

    u64 next_read = 0;
    u64 next_write = 0;
    bool eof = false;
    bool have_staged_header = false;
    BlockHeader staged_h;
    u64 staged_need = 0;

    auto write_task = [&](DecompressTask& t) {
        out.write(t.out.data(), t.out.size());
        st.raw_bytes += t.out.size();
        st.blocks += 1;
        st.stored_blocks += t.h.stages.empty() ? 1 : 0;
        t.out = {};
        t.payload = {};
    };

    try {
        while (!eof || next_write < next_read) {
            // 1. Drain any contiguous ready blocks in index order.
            for (;;) {
                std::unique_ptr<DecompressTask> done;
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    if (state.aborted) break;
                    done = state.take_ready_locked(next_write);
                }
                if (!done) break;
                const u64 cost = done->mem_cost;
                write_task(*done);
                ++next_write;
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.in_flight_mem -= cost;
                    --state.in_flight_blocks;
                }
            }

            {
                std::lock_guard<std::mutex> lock(state.mu);
                if (state.aborted) break;
            }

            // 2. Read block headers and payloads while within memory and concurrency limits.
            bool read_any = false;
            while (!eof) {
                if (!have_staged_header) {
                    if (!ar.next_block(staged_h)) {
                        eof = true;
                        std::lock_guard<std::mutex> lock(state.mu);
                        state.input_done = true;
                        state.cv_worker.notify_all();
                        break;
                    }
                    staged_need =
                        staged_h.stages.empty() ? staged_h.raw_size : block_memory_estimate(staged_h.raw_size);
                    if (staged_need > opt.mem_limit)
                        fail("block " + std::to_string(next_read) + " needs about " +
                             std::to_string(staged_need >> 20) +
                             " MiB to decode, above the memory limit (raise it with -M)");
                    have_staged_header = true;
                }

                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    if (state.aborted) break;
                    const bool can_admit =
                        state.in_flight_blocks == 0 ||
                        (state.in_flight_blocks < max_in_flight &&
                         state.in_flight_mem + staged_need <= opt.mem_limit);
                    if (!can_admit) break;
                }

                auto task = std::make_unique<DecompressTask>();
                task->index = next_read++;
                task->mem_cost = staged_need;
                task->h = std::move(staged_h);
                have_staged_header = false;
                ar.read_payload(task->payload, task->h.comp_size);

                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.in_flight_mem += task->mem_cost;
                    ++state.in_flight_blocks;
                    state.pending.push_back(std::move(task));
                }
                state.cv_worker.notify_one();
                read_any = true;
            }

            // 3. Wait for the next sequential block to finish if we cannot read more right now.
            if (!read_any && next_write < next_read) {
                std::unique_ptr<DecompressTask> done;
                {
                    std::unique_lock<std::mutex> lock(state.mu);
                    state.cv_coord.wait(lock, [&] {
                        if (state.aborted) return true;
                        done = state.take_ready_locked(next_write);
                        return done != nullptr;
                    });
                    if (state.aborted) break;
                }
                if (done) {
                    const u64 cost = done->mem_cost;
                    write_task(*done);
                    ++next_write;
                    {
                        std::lock_guard<std::mutex> lock(state.mu);
                        state.in_flight_mem -= cost;
                        --state.in_flight_blocks;
                    }
                }
            }
        }
    } catch (...) {
        state.record_error(std::current_exception());
    }

    {
        std::lock_guard<std::mutex> lock(state.mu);
        state.input_done = true;
        state.cv_worker.notify_all();
    }
    pool.join_all();

    if (state.first_error) std::rethrow_exception(state.first_error);

    st.archive_bytes = ar.consumed();
    return st;
}

// ---------------------------------------------------------------- list

ArchiveInfo list_archive(Input& in) {
    ArchiveReader ar(in);
    ArchiveInfo info;
    info.format_major = ar.format_major;
    info.format_minor = ar.format_minor;
    info.block_log2 = ar.block_log2;

    BlockHeader h;
    while (ar.next_block(h)) {
        BlockInfo b;
        b.raw_size = h.raw_size;
        b.comp_size = h.comp_size;
        b.hash = h.hash;
        for (const StageDesc& s : h.stages) b.stage_ids.push_back(s.id);
        info.raw_bytes += h.raw_size;
        info.blocks.push_back(std::move(b));
        ar.skip_payload(h.comp_size);
    }
    info.archive_bytes = ar.consumed();
    return info;
}

}  // namespace apx
