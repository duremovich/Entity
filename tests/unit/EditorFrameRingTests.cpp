#include "entity/render/EditorFrameRing.hpp"
#include <gtest/gtest.h>

using entity::render::EditorFrameRing;

// The bug: ImGui keys its buffer ring on a monotonic counter, our old fence ring
// keyed on the swap-chain index. Under a no-flip present the swap-chain index
// repeats while ImGui's counter advances. The ring under test must key on the
// SAME monotonic counter ImGui uses, so the fenced slot always equals ImGui's
// reuse slot regardless of what the swap chain reports.
TEST(EditorFrameRing, FencedSlotMatchesImGuiSlotUnderNoFlipPresent) {
    constexpr uint32_t N = 2; // NumFramesInFlight
    EditorFrameRing ring(N);

    // Simulate ImGui's free-running counter (init UINT_MAX, ++ before use).
    uint32_t imguiCounter = UINT32_MAX;
    auto imguiSlot = [&]() { ++imguiCounter; return imguiCounter % N; };

    // Frame 0: swap-chain index 0.
    ring.beginFrame(/*swapChainIndex=*/0);
    EXPECT_EQ(ring.currentSlot(), imguiSlot());   // both -> 0
    ring.endFrame();

    // Frame 1: swap-chain index 1 (normal flip).
    ring.beginFrame(/*swapChainIndex=*/1);
    EXPECT_EQ(ring.currentSlot(), imguiSlot());   // both -> 1
    ring.endFrame();

    // Frame 2: NO-FLIP present — swap chain repeats index 1.
    // The OLD code would pick slot 1 again; ImGui advances to slot 0.
    // The fix keys on the monotonic counter, so the ring must also pick 0.
    ring.beginFrame(/*swapChainIndex=*/1);
    EXPECT_EQ(ring.currentSlot(), imguiSlot());   // ImGui -> 0; ring MUST -> 0
    ring.endFrame();
}

TEST(EditorFrameRing, WaitTargetIsPriorUseOfSameSlot) {
    constexpr uint32_t N = 2;
    EditorFrameRing ring(N);
    ring.beginFrame(0); ring.endFrame();           // slot 0 used, fence value V0
    ring.beginFrame(1); ring.endFrame();           // slot 1 used, fence value V1
    // Re-entering slot 0: the wait target must be slot 0's prior signaled value,
    // not slot 1's, so ImGui's grow-and-free of slot 0 is fenced.
    ring.beginFrame(0);
    EXPECT_EQ(ring.pendingWaitValue(), ring.lastSignaledForSlot(0));
}
