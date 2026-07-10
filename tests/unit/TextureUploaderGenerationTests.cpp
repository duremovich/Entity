/**
 * Unit tests for TextureUploader's per-slot reuse generation (#90).
 *
 * The generation counter is the cross-tick defense against the stale-cached-
 * snapshot hazard: the editor frees a video-texture slot and a different clip
 * first-fits the same index, while the show thread still holds a SceneSnapshot
 * mapping the old clip to that slot. Each free→realloc cycle bumps the slot's
 * generation; an upload/draw carrying the pre-free generation is rejected.
 *
 * These tests exercise the generation protocol directly — TextureUploader is
 * default-constructible without a D3D12 device, and allocateSlot / freeSlot /
 * slotGeneration touch only m_slots + m_slotMutex (no GPU resources). The
 * upload() rejection itself needs a device and is covered by the integration
 * scenario (video_texture_slot_reuse_stale_snapshot). Style mirrors
 * EditorReadGateTests.cpp, including the concurrent-mutation stress test.
 */

#include <gtest/gtest.h>

#include "entity/render/TextureUploader.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace entity;

namespace {

TEST(TextureUploaderGeneration, GenerationStartsZero) {
    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    ASSERT_NE(slot, TextureUploader::INVALID_SLOT);
    EXPECT_EQ(up.slotGeneration(slot), 0u);
}

TEST(TextureUploaderGeneration, AllocateDoesNotBump) {
    TextureUploader up;
    const uint32_t a = up.allocateSlot();
    const uint32_t b = up.allocateSlot();
    ASSERT_NE(a, TextureUploader::INVALID_SLOT);
    ASSERT_NE(b, TextureUploader::INVALID_SLOT);
    // A plain allocate (no preceding free) never bumps — the first occupant of
    // a fresh index sees generation 0.
    EXPECT_EQ(up.slotGeneration(a), 0u);
    EXPECT_EQ(up.slotGeneration(b), 0u);
}

TEST(TextureUploaderGeneration, FreeBumpsGeneration) {
    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    ASSERT_EQ(slot, 0u);  // first-fit hands out the lowest free index
    EXPECT_EQ(up.slotGeneration(slot), 0u);

    up.freeSlot(slot);
    EXPECT_EQ(up.slotGeneration(slot), 1u);  // one bump per free

    const uint32_t reslot = up.allocateSlot();
    EXPECT_EQ(reslot, slot);                 // first-fit reuses the same index
    EXPECT_EQ(up.slotGeneration(reslot), 1u); // realloc does NOT bump again
}

TEST(TextureUploaderGeneration, StaleGenerationRejectedLiveAccepted) {
    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    ASSERT_NE(slot, TextureUploader::INVALID_SLOT);

    // Clip A captures the slot's generation at snapshot-bake time.
    const uint32_t capturedByA = up.slotGeneration(slot);

    // Editor destroys A and a new clip B first-fits the same slot index.
    up.freeSlot(slot);
    const uint32_t reslot = up.allocateSlot();
    ASSERT_EQ(reslot, slot);

    // A's captured generation is now stale (would be rejected); a generation
    // read fresh for B is live (would be accepted).
    EXPECT_NE(capturedByA, up.slotGeneration(slot));
    const uint32_t capturedByB = up.slotGeneration(reslot);
    EXPECT_EQ(capturedByB, up.slotGeneration(slot));
}

// planUpload is the locked verdict the renderer branches on (F3). It shares
// upload()'s reject predicate, so a stale generation both rejects AND counts
// here (F5) — the whole point of centralizing the check. No device needed:
// planUpload touches only m_slots + m_slotMutex.
TEST(TextureUploaderGeneration, PlanUploadRejectsStaleAndCounts) {
    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    const uint32_t captured = up.slotGeneration(slot);

    up.freeSlot(slot);
    up.allocateSlot();  // same index, generation bumped

    EXPECT_EQ(up.staleGenerationSkips(), 0u);
    const auto plan = up.planUpload(slot, 64, 64, TextureFormat::RGBA8_UNORM, captured);
    EXPECT_TRUE(plan.reject);
    EXPECT_FALSE(plan.needsResizeSync);
    EXPECT_EQ(up.staleGenerationSkips(), 1u);  // the shared predicate counted the reject
}

TEST(TextureUploaderGeneration, PlanUploadAcceptsLiveGeneration) {
    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    const auto plan = up.planUpload(slot, 64, 64, TextureFormat::RGBA8_UNORM,
                                    up.slotGeneration(slot));
    EXPECT_FALSE(plan.reject);
    // No device → no texture ever created → the resize-sync path is never taken.
    EXPECT_FALSE(plan.needsResizeSync);
    EXPECT_EQ(up.staleGenerationSkips(), 0u);
}

TEST(TextureUploaderGeneration, PlanUploadSkipSentinelNeverRejects) {
    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    up.freeSlot(slot);
    up.allocateSlot();  // generation bumped, but the caller passes skip
    const auto plan = up.planUpload(slot, 64, 64, TextureFormat::RGBA8_UNORM,
                                    TextureUploader::kSkipGenerationCheck);
    EXPECT_FALSE(plan.reject);
    EXPECT_EQ(up.staleGenerationSkips(), 0u);
}

// Stress: one thread cycles allocate/free on a fixed slot index while another
// does lock-free relaxed slotGeneration() reads. The reader asserts it never
// observes a decreasing (torn) generation — the counter is monotonic because
// the sole writer only ever increments it, and the store is ordered by
// m_slotMutex in freeSlot. A non-atomic generation field would let the relaxed
// cross-thread read tear under load (UB), which this test is built to catch.
TEST(TextureUploaderGeneration, StressNoTornGenerationUnderConcurrentMutation) {
    constexpr int CYCLES = 20000;

    TextureUploader up;
    const uint32_t slot = up.allocateSlot();
    ASSERT_EQ(slot, 0u);  // first-fit keeps returning this index across cycles

    std::atomic<bool> done{false};
    std::atomic<bool> violated{false};

    std::thread reader([&] {
        uint32_t last = 0;
        while (!done.load(std::memory_order_relaxed)) {
            const uint32_t g = up.slotGeneration(slot);
            if (g < last) {
                violated.store(true, std::memory_order_relaxed);
            }
            last = g;
        }
    });

    std::thread mutator([&] {
        for (int i = 0; i < CYCLES; ++i) {
            up.freeSlot(slot);
            const uint32_t s = up.allocateSlot();  // first-fit → same index
            (void)s;
        }
        done.store(true, std::memory_order_relaxed);
    });

    mutator.join();
    reader.join();

    EXPECT_FALSE(violated.load())
        << "slotGeneration() observed a decreasing (torn) value";
    // Each free bumped once; realloc never bumps — so the final generation is
    // exactly the number of free→realloc cycles.
    EXPECT_EQ(up.slotGeneration(slot), static_cast<uint32_t>(CYCLES));
}

} // namespace
