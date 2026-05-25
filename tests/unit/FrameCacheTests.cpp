/**
 * Unit tests for entity::FrameCache.
 *
 * Goals:
 *   - Lock down the contract (put/get/has/nearestTo/evictClip).
 *   - Gate LRU semantics — recent puts win, old entries fall out under
 *     pressure, MRU promotion on get keeps hot frames pinned.
 *   - Gate the lease lifetime — the cache forgetting an entry must not
 *     free its bytes while a reader still holds the lease.
 *   - Smoke producer/consumer concurrency to catch obvious race regressions.
 */

#include <gtest/gtest.h>

#include "entity/media/FrameCache.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace entity;

namespace {

DecodedFrame makeFrame(FrameNumber n, size_t bytes, uint8_t fill = 0) {
    DecodedFrame f;
    f.frameNumber = n;
    f.width  = 64;
    f.height = 64;
    f.format = TextureFormat::RGBA8_UNORM;
    f.storage = DecodedFrame::Storage::CpuHeap;
    f.cpuData.assign(bytes, fill);
    f.valid.store(true, std::memory_order_release);
    return f;
}

// All tests use synthetic entt::entity values cast from integers — the cache
// only inspects the underlying handle as a hash key, so this is safe and
// avoids needing a registry.
constexpr entt::entity kClipA{static_cast<entt::entity>(0x1001)};
constexpr entt::entity kClipB{static_cast<entt::entity>(0x1002)};

} // namespace

TEST(FrameCache, EmptyCacheHasNothing) {
    FrameCache c(/*maxBytes=*/1024);
    EXPECT_EQ(c.entryCount(), 0u);
    EXPECT_EQ(c.bytesUsed(), 0u);
    EXPECT_FALSE(c.has(kClipA, 0));
    EXPECT_FALSE(c.get(kClipA, 0).valid());
    EXPECT_FALSE(c.nearestTo(kClipA, 100).has_value());
}

TEST(FrameCache, PutThenGetRoundTrips) {
    FrameCache c(1024);
    c.put(kClipA, 5, makeFrame(5, 128, 0xAB));

    EXPECT_TRUE(c.has(kClipA, 5));
    EXPECT_EQ(c.entryCount(), 1u);
    EXPECT_EQ(c.bytesUsed(), 128u);

    auto lease = c.get(kClipA, 5);
    ASSERT_TRUE(lease.valid());
    EXPECT_EQ(lease->frameNumber, 5);
    EXPECT_EQ(lease->size(), 128u);
    EXPECT_EQ(lease->data()[0], 0xAB);
}

TEST(FrameCache, ZeroByteFrameIsRejected) {
    // A zero-byte frame is almost always a decode error; caching one would
    // mean future cache hits upload empty buffers.
    FrameCache c(1024);
    c.put(kClipA, 5, makeFrame(5, 0));
    EXPECT_FALSE(c.has(kClipA, 5));
    EXPECT_EQ(c.entryCount(), 0u);
}

TEST(FrameCache, DisabledCacheIsNoOp) {
    // maxBytes==0 disables — every put silently drops, every get misses.
    // Caller still functions; decoder gets re-asked.
    FrameCache c(0);
    c.put(kClipA, 5, makeFrame(5, 128));
    EXPECT_FALSE(c.has(kClipA, 5));
    EXPECT_EQ(c.entryCount(), 0u);
    EXPECT_EQ(c.bytesUsed(), 0u);
}

TEST(FrameCache, ReinsertReplacesAndKeepsByteCountConsistent) {
    FrameCache c(1024);
    c.put(kClipA, 5, makeFrame(5, 100, 0x11));
    c.put(kClipA, 5, makeFrame(5, 200, 0x22));

    EXPECT_EQ(c.entryCount(), 1u);
    EXPECT_EQ(c.bytesUsed(), 200u);

    auto lease = c.get(kClipA, 5);
    ASSERT_TRUE(lease.valid());
    EXPECT_EQ(lease->size(), 200u);
    EXPECT_EQ(lease->data()[0], 0x22);
}

TEST(FrameCache, LRUEvictsOldestUnderPressure) {
    // Budget = 300 bytes. Inserting four 100-byte frames evicts the oldest.
    FrameCache c(300);
    c.put(kClipA, 0, makeFrame(0, 100));
    c.put(kClipA, 1, makeFrame(1, 100));
    c.put(kClipA, 2, makeFrame(2, 100));
    EXPECT_EQ(c.entryCount(), 3u);
    EXPECT_EQ(c.bytesUsed(), 300u);

    c.put(kClipA, 3, makeFrame(3, 100));
    EXPECT_EQ(c.entryCount(), 3u);
    EXPECT_EQ(c.bytesUsed(), 300u);
    EXPECT_FALSE(c.has(kClipA, 0)); // oldest evicted
    EXPECT_TRUE(c.has(kClipA, 1));
    EXPECT_TRUE(c.has(kClipA, 2));
    EXPECT_TRUE(c.has(kClipA, 3));
}

TEST(FrameCache, GetPromotesMRUAndPreventsEviction) {
    FrameCache c(300);
    c.put(kClipA, 0, makeFrame(0, 100));
    c.put(kClipA, 1, makeFrame(1, 100));
    c.put(kClipA, 2, makeFrame(2, 100));

    // Touch frame 0 — it becomes MRU. Inserting frame 3 should evict
    // frame 1, not frame 0.
    {
        auto lease = c.get(kClipA, 0);
        ASSERT_TRUE(lease.valid());
    }

    c.put(kClipA, 3, makeFrame(3, 100));
    EXPECT_TRUE(c.has(kClipA, 0));  // pinned by recent get
    EXPECT_FALSE(c.has(kClipA, 1)); // evicted
    EXPECT_TRUE(c.has(kClipA, 2));
    EXPECT_TRUE(c.has(kClipA, 3));
}

TEST(FrameCache, EvictClipDropsOnlyMatchingEntries) {
    FrameCache c(10000);
    c.put(kClipA, 0, makeFrame(0, 100));
    c.put(kClipA, 1, makeFrame(1, 100));
    c.put(kClipB, 0, makeFrame(0, 100));
    c.put(kClipB, 1, makeFrame(1, 100));

    EXPECT_EQ(c.entryCount(), 4u);
    EXPECT_EQ(c.bytesUsed(), 400u);

    c.evictClip(kClipA);

    EXPECT_EQ(c.entryCount(), 2u);
    EXPECT_EQ(c.bytesUsed(), 200u);
    EXPECT_FALSE(c.has(kClipA, 0));
    EXPECT_FALSE(c.has(kClipA, 1));
    EXPECT_TRUE(c.has(kClipB, 0));
    EXPECT_TRUE(c.has(kClipB, 1));
}

TEST(FrameCache, NearestToReturnsClosest) {
    FrameCache c(10000);
    c.put(kClipA, 10, makeFrame(10, 100));
    c.put(kClipA, 20, makeFrame(20, 100));
    c.put(kClipA, 35, makeFrame(35, 100));
    c.put(kClipB, 100, makeFrame(100, 100)); // wrong clip — must be ignored

    EXPECT_EQ(c.nearestTo(kClipA, 9).value(),  10); // closest below
    EXPECT_EQ(c.nearestTo(kClipA, 14).value(), 10);
    EXPECT_EQ(c.nearestTo(kClipA, 16).value(), 20); // tie-break: first-seen wins; 16 is exactly 4 from 20 and 6 from 10, so 20
    EXPECT_EQ(c.nearestTo(kClipA, 20).value(), 20);
    EXPECT_EQ(c.nearestTo(kClipA, 28).value(), 35);
    EXPECT_EQ(c.nearestTo(kClipA, 99).value(), 35);

    // Empty clip → no answer.
    EXPECT_FALSE(c.nearestTo(static_cast<entt::entity>(0x9999), 0).has_value());
}

TEST(FrameCache, LeasePinsFrameThroughEviction) {
    // Cache forgets the entry, but the lease keeps the bytes alive. This
    // is the property that lets the GPU upload path drop the cache mutex
    // before memcpy'ing 33 MB of pixel data.
    FrameCache c(100);
    c.put(kClipA, 0, makeFrame(0, 100, 0x77));

    auto lease = c.get(kClipA, 0);
    ASSERT_TRUE(lease.valid());

    // Grab a second pointer to confirm the bytes survive the eviction.
    const DecodedFrame* raw = lease.get();
    ASSERT_NE(raw, nullptr);

    // Force eviction by inserting another full-budget entry.
    c.put(kClipA, 1, makeFrame(1, 100));
    EXPECT_FALSE(c.has(kClipA, 0));        // cache forgot it
    EXPECT_TRUE(c.has(kClipA, 1));
    EXPECT_EQ(c.bytesUsed(), 100u);        // budget reflects what's in cache, not lease

    // Lease still works — the underlying buffer hasn't been freed.
    EXPECT_EQ(raw->size(), 100u);
    EXPECT_EQ(raw->data()[0], 0x77);
    EXPECT_EQ(lease->frameNumber, 0);
}

TEST(FrameCache, SetMaxBytesShrinksImmediately) {
    FrameCache c(1000);
    for (int i = 0; i < 8; ++i) {
        c.put(kClipA, i, makeFrame(i, 100));
    }
    EXPECT_EQ(c.entryCount(), 8u);
    EXPECT_EQ(c.bytesUsed(), 800u);

    // Shrink to 250 bytes — should retain only the 2 most-recent.
    c.setMaxBytes(250);
    EXPECT_EQ(c.bytesUsed(), 200u);
    EXPECT_EQ(c.entryCount(), 2u);
    EXPECT_TRUE(c.has(kClipA, 7));
    EXPECT_TRUE(c.has(kClipA, 6));
    EXPECT_FALSE(c.has(kClipA, 5));
}

TEST(FrameCache, SetMaxBytesGrowKeepsExistingEntries) {
    FrameCache c(300);
    c.put(kClipA, 0, makeFrame(0, 100));
    c.put(kClipA, 1, makeFrame(1, 100));

    c.setMaxBytes(2 * 1024 * 1024); // grow
    EXPECT_EQ(c.entryCount(), 2u);
    EXPECT_EQ(c.bytesUsed(), 200u);
    EXPECT_TRUE(c.has(kClipA, 0));
    EXPECT_TRUE(c.has(kClipA, 1));
}

TEST(FrameCache, ConcurrentProducerAndConsumerSmoke) {
    // Smoke test only — not a guarantee of correctness under all schedulings,
    // but TSan + this hammering catches the obvious "I forgot a lock" bug.
    FrameCache c(1 * 1024 * 1024); // 1 MB

    constexpr int kIters = 2000;
    constexpr size_t kFrameBytes = 1024;

    std::atomic<bool> stop{false};

    std::thread producer([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            c.put(kClipA, i, makeFrame(i, kFrameBytes,
                                       static_cast<uint8_t>(i & 0xFF)));
            if ((i & 0x1F) == 0) std::this_thread::yield();
        }
    });

    std::atomic<int> hits{0};
    std::thread consumer([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            auto lease = c.get(kClipA, i);
            if (lease.valid()) {
                hits.fetch_add(1);
                // Touch the data to make TSan/ASan flag a use-after-free.
                volatile uint8_t v = (lease->size() == 0) ? 0 : lease->data()[0];
                (void)v;
            }
            (void)c.nearestTo(kClipA, i);
            if ((i & 0x1F) == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    // We don't assert on hit count — the consumer may race ahead of the
    // producer at any given iteration. The point is: no crash, no UAF, no
    // accounting drift.
    EXPECT_LE(c.bytesUsed(), c.maxBytes());
    EXPECT_EQ(c.entryCount() * kFrameBytes, c.bytesUsed());
}
