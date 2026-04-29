#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace entity {

// Buffer pool for decoded-frame pixel buffers, keyed by exact byte size.
//
// Why this exists: the decode worker (DecodeSystem.cpp) does
// `cache.put(std::move(frame))` every iteration. The std::move steals the
// worker's pixel-buffer heap allocation; the worker's `frame.data` is empty
// next iteration; the previous code path called `frame.data.resize(N)` to
// reallocate. For 4K ProRes 4444 RGBA8 (~37.7 MB/frame at 30 fps), that's
// ~1.13 GB/s of HeapAlloc churn per layer plus mandatory zero-init the
// decoder immediately overwrites. On Windows, allocations of that size go
// through VirtualAlloc with variable 0.1-50 ms latency under fragmentation
// or memory pressure, which manifests as stuttering playback.
//
// The pool replaces that pattern: workers `acquire(bytes)` pre-sized
// buffers from the bucket of that exact size; FrameCache wraps each
// stored frame in a `shared_ptr<DecodedFrame>` whose deleter calls
// `release(...)` on the buffer when the cache evicts the entry (last
// strong ref drops). Steady-state -- after one round-trip of cache fill
// + first eviction -- there is zero pixel-buffer malloc.
//
// Sizing: per-bucket cap (default 4 buffers) limits over-supply when a
// single resolution dominates; global cap (default 8 buffers across all
// buckets) bounds slack memory across mixed-resolution projects.
//
// Thread-safety: a single mutex protects the buckets map. Workers
// acquire from any thread; deleters release from any thread (including
// late-firing on the GPU upload path's lease destructor). No path
// re-enters the cache from inside `release`, so the lock order stays
// `cache -> pool`, never the reverse.
//
// Lifetime safety: FrameCache holds a `weak_ptr<DecodeBufferPool>`. If
// the pool is destroyed before an outstanding `FrameLease` drops, the
// deleter's `weak_ptr.lock()` returns null and the buffer falls through
// to plain `delete`. No use-after-free across teardown.
class DecodeBufferPool {
public:
    // Defaults are tuned for the common case: a project full of one
    // resolution rarely accumulates more than `maxBuffersPerSize`
    // recyclable buffers in flight; mixed-resolution projects bound
    // total slack memory to roughly `maxTotal * largest_frame_size`.
    explicit DecodeBufferPool(std::size_t maxBuffersPerSize = 4,
                              std::size_t maxTotal = 8);
    ~DecodeBufferPool() = default;

    DecodeBufferPool(const DecodeBufferPool&) = delete;
    DecodeBufferPool& operator=(const DecodeBufferPool&) = delete;

    // Pop a buffer with `size() == bytes` from the matching bucket. The
    // bytes are NOT zero-initialized on a hit (the decoder will
    // overwrite them); on a miss, a fresh `std::vector(bytes)` is
    // value-initialized as usual (one-time cost on cold start).
    std::vector<std::uint8_t> acquire(std::size_t bytes);

    // Push a buffer onto its size-bucket. Drops if the per-bucket cap
    // or global cap is exceeded. The buffer is std::move'd in; do not
    // touch the source after calling.
    void release(std::vector<std::uint8_t>&& buf);

    // Diagnostics.
    std::size_t entries() const;
    std::size_t bucketCount() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::size_t, std::vector<std::vector<std::uint8_t>>> m_buckets;
    std::size_t m_maxBuffersPerSize;
    std::size_t m_maxTotal;
    std::size_t m_totalEntries{0};
};

} // namespace entity
