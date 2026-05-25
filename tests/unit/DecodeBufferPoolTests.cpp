#include "entity/media/DecodeBufferPool.hpp"
#include "entity/media/FrameCache.hpp"

#include <gtest/gtest.h>

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

using entity::DecodeBufferPool;
using entity::FrameCache;
using entity::FrameLease;
using entity::DecodedFrame;

// ---------------------------------------------------------------------------
// Pool fundamentals
// ---------------------------------------------------------------------------

TEST(DecodeBufferPoolTest, AcquireAfterReleaseRecyclesSameAllocation) {
    DecodeBufferPool pool;
    std::vector<std::uint8_t> buf(1024);
    const auto* originalPtr = buf.data();
    pool.release(std::move(buf));

    auto recycled = pool.acquire(1024);
    EXPECT_EQ(recycled.size(), 1024u);
    // Steady-state contract: acquire on a hit returns the previously
    // released allocation (same data() pointer). This is the property
    // that eliminates per-frame malloc.
    EXPECT_EQ(recycled.data(), originalPtr);
}

TEST(DecodeBufferPoolTest, AcquireOnEmptyPoolMallocsFresh) {
    DecodeBufferPool pool;
    auto buf = pool.acquire(2048);
    EXPECT_EQ(buf.size(), 2048u);
    // No prior release; just verify size and allocation succeeded.
    EXPECT_NE(buf.data(), nullptr);
}

TEST(DecodeBufferPoolTest, PerBucketCapHonored) {
    // Per-bucket cap = 2; release 4 buffers into the same bucket → only
    // 2 are kept, the rest free as the rvalues drop.
    DecodeBufferPool pool(/*maxBuffersPerSize=*/2, /*maxTotal=*/100);
    pool.release(std::vector<std::uint8_t>(512));
    pool.release(std::vector<std::uint8_t>(512));
    pool.release(std::vector<std::uint8_t>(512));
    pool.release(std::vector<std::uint8_t>(512));
    EXPECT_EQ(pool.entries(), 2u);
}

TEST(DecodeBufferPoolTest, GlobalCapHonored) {
    // Global cap = 3; release 5 buffers across many buckets → only 3 kept.
    DecodeBufferPool pool(/*maxBuffersPerSize=*/4, /*maxTotal=*/3);
    pool.release(std::vector<std::uint8_t>(100));
    pool.release(std::vector<std::uint8_t>(200));
    pool.release(std::vector<std::uint8_t>(300));
    pool.release(std::vector<std::uint8_t>(400));
    pool.release(std::vector<std::uint8_t>(500));
    EXPECT_EQ(pool.entries(), 3u);
}

TEST(DecodeBufferPoolTest, DifferentSizesUseSeparateBuckets) {
    DecodeBufferPool pool;
    pool.release(std::vector<std::uint8_t>(100));
    pool.release(std::vector<std::uint8_t>(200));

    // Acquiring size 100 must NOT pop the size-200 buffer (different bucket).
    auto buf100 = pool.acquire(100);
    EXPECT_EQ(buf100.size(), 100u);

    // Size-200 bucket still has its entry; acquiring 200 hits.
    EXPECT_EQ(pool.entries(), 1u);
    auto buf200 = pool.acquire(200);
    EXPECT_EQ(buf200.size(), 200u);
    EXPECT_EQ(pool.entries(), 0u);
}

TEST(DecodeBufferPoolTest, AcquireFromEmptyBucketWhenOtherBucketHasEntries) {
    DecodeBufferPool pool;
    pool.release(std::vector<std::uint8_t>(100));
    // Bucket for size-200 is empty even though pool has a size-100 entry.
    auto buf = pool.acquire(200);
    EXPECT_EQ(buf.size(), 200u);
    // Size-100 entry untouched.
    EXPECT_EQ(pool.entries(), 1u);
}

TEST(DecodeBufferPoolTest, ReleasingEmptyBufferIsNoOp) {
    DecodeBufferPool pool;
    pool.release(std::vector<std::uint8_t>{});
    EXPECT_EQ(pool.entries(), 0u);
}

// ---------------------------------------------------------------------------
// FrameCache integration -- the eviction-recycles-buffer path.
// ---------------------------------------------------------------------------

namespace {

DecodedFrame makeFrame(std::size_t bytes, entity::FrameNumber frameNum) {
    DecodedFrame f;
    f.frameNumber = frameNum;
    f.width = 1;
    f.height = 1;
    // Explicitly use the CpuHeap path so this test helper doesn't depend on
    // D3D12 availability. Storage::CpuHeap is the default; set explicitly for
    // clarity.
    f.storage = DecodedFrame::Storage::CpuHeap;
    f.cpuData.resize(bytes);
    f.valid.store(true);
    return f;
}

} // namespace

TEST(DecodeBufferPoolTest, CacheEvictionReturnsBufferToPool) {
    auto pool = std::make_shared<DecodeBufferPool>();
    FrameCache cache(/*maxBytes=*/128);
    cache.setBufferPool(pool);

    const entt::entity clip{1};

    // Two 100-byte frames into a 128-byte cache: the second eviction
    // tail-chops the first, dropping its shared_ptr to ref-count zero,
    // firing the deleter, recycling its buffer to the pool.
    cache.put(clip, 0, makeFrame(100, 0));
    EXPECT_EQ(pool->entries(), 0u);  // no eviction yet

    cache.put(clip, 1, makeFrame(100, 1));
    // First entry was evicted; deleter ran; pool now holds its buffer.
    EXPECT_EQ(pool->entries(), 1u);

    // Acquire from the pool: must return a 100-byte buffer.
    auto recycled = pool->acquire(100);
    EXPECT_EQ(recycled.size(), 100u);
    EXPECT_EQ(pool->entries(), 0u);
}

TEST(DecodeBufferPoolTest, LeaseOutlivingPoolDestructionFallsThroughToDelete) {
    // Safety property: if the pool is destroyed before an outstanding
    // FrameLease drops, the deleter's weak_ptr.lock() returns null and
    // the buffer falls through to `delete p` -- no crash, no use-after-
    // free. This test crashes (sanitized build) or ASans-fails if the
    // deleter dereferences a dead pool.
    FrameCache cache(/*maxBytes=*/1024);
    {
        auto pool = std::make_shared<DecodeBufferPool>();
        cache.setBufferPool(pool);
        cache.put(entt::entity{1}, 0, makeFrame(64, 0));
        // pool goes out of scope here -- the cache's weak_ptr will fail
        // to lock when the entry is evicted on cache destruction.
    }
    // Hold a lease past the pool's destruction.
    auto lease = cache.get(entt::entity{1}, 0);
    EXPECT_TRUE(lease.valid());

    // Drop the cache (deleters fire on remaining entries; pool already
    // gone, weak_ptr.lock() returns null, buffer just frees).
    cache.evictClip(entt::entity{1});

    // Lease still valid since shared_ptr ref count is non-zero.
    EXPECT_TRUE(lease.valid());

    // Lease drops at end of scope; deleter runs, weak_ptr.lock() fails,
    // buffer freed via plain delete. No assertion, just non-crash.
}

TEST(DecodeBufferPoolTest, CacheWithoutPoolStillWorks) {
    // Backwards-compat: the cache is fully functional without a pool
    // wired (the pool is an optional perf optimization). Without
    // setBufferPool, eviction just frees buffers normally.
    FrameCache cache(/*maxBytes=*/128);
    cache.put(entt::entity{1}, 0, makeFrame(100, 0));
    cache.put(entt::entity{1}, 1, makeFrame(100, 1));
    // Frame 0 was evicted and freed (no pool to receive it). No crash.
    EXPECT_FALSE(cache.has(entt::entity{1}, 0));
    EXPECT_TRUE(cache.has(entt::entity{1}, 1));
}
