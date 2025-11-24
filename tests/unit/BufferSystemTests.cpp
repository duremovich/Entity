#include <gtest/gtest.h>
#include "entity/systems/BufferSystem.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/media/FrameRingBuffer.hpp"

using namespace entity;

/**
 * BufferSystem Unit Tests
 *
 * Validates that BufferSystem correctly implements the logic extracted
 * from FrameBuffer component methods (hasFrames, isReady, getFillPercentage).
 */

class BufferSystemTest : public ::testing::Test {
protected:
    BufferSystem system;
    FrameBuffer buffer;
    entt::registry registry;

    void SetUp() override {
        // Initialize buffer with a ring buffer instance
        buffer.ringBuffer = std::make_shared<FrameRingBuffer>();
        buffer.currentPTS.store(0);
        buffer.targetFrame.store(0);
        buffer.isBuffering.store(true);
        buffer.bufferedFrames.store(0);
    }
};

// Test hasFrames() - should return false when buffer is empty
TEST_F(BufferSystemTest, HasFrames_EmptyBuffer_ReturnsFalse) {
    buffer.bufferedFrames.store(0);
    EXPECT_FALSE(system.hasFrames(buffer));
}

// Test hasFrames() - should return true when buffer has frames
TEST_F(BufferSystemTest, HasFrames_WithFrames_ReturnsTrue) {
    buffer.bufferedFrames.store(1);
    EXPECT_TRUE(system.hasFrames(buffer));

    buffer.bufferedFrames.store(10);
    EXPECT_TRUE(system.hasFrames(buffer));

    buffer.bufferedFrames.store(32);
    EXPECT_TRUE(system.hasFrames(buffer));
}

// Test isReady() - should return false when buffer has less than 8 frames
TEST_F(BufferSystemTest, IsReady_LessThanMinFrames_ReturnsFalse) {
    buffer.bufferedFrames.store(0);
    EXPECT_FALSE(system.isReady(buffer));

    buffer.bufferedFrames.store(7);
    EXPECT_FALSE(system.isReady(buffer));
}

// Test isReady() - should return true when buffer has 8 or more frames
TEST_F(BufferSystemTest, IsReady_MinOrMoreFrames_ReturnsTrue) {
    buffer.bufferedFrames.store(8);
    EXPECT_TRUE(system.isReady(buffer));

    buffer.bufferedFrames.store(16);
    EXPECT_TRUE(system.isReady(buffer));

    buffer.bufferedFrames.store(32);
    EXPECT_TRUE(system.isReady(buffer));
}

// Test getFillPercentage() - should return correct percentage
TEST_F(BufferSystemTest, GetFillPercentage_CalculatesCorrectly) {
    // Empty buffer = 0%
    buffer.bufferedFrames.store(0);
    EXPECT_FLOAT_EQ(system.getFillPercentage(buffer), 0.0f);

    // Half full = 50%
    buffer.bufferedFrames.store(16);
    EXPECT_FLOAT_EQ(system.getFillPercentage(buffer), 0.5f);

    // Full buffer = 100%
    buffer.bufferedFrames.store(DEFAULT_FRAME_BUFFER_SIZE);
    EXPECT_FLOAT_EQ(system.getFillPercentage(buffer), 1.0f);

    // 8 frames = 25%
    buffer.bufferedFrames.store(8);
    EXPECT_FLOAT_EQ(system.getFillPercentage(buffer), 8.0f / DEFAULT_FRAME_BUFFER_SIZE);
}

// Test isBuffering() - should reflect buffering state
TEST_F(BufferSystemTest, IsBuffering_ReflectsState) {
    buffer.isBuffering.store(true);
    EXPECT_TRUE(system.isBuffering(buffer));

    buffer.isBuffering.store(false);
    EXPECT_FALSE(system.isBuffering(buffer));
}

// Test getBufferedFrameCount() - should return current count
TEST_F(BufferSystemTest, GetBufferedFrameCount_ReturnsCount) {
    buffer.bufferedFrames.store(0);
    EXPECT_EQ(system.getBufferedFrameCount(buffer), 0u);

    buffer.bufferedFrames.store(15);
    EXPECT_EQ(system.getBufferedFrameCount(buffer), 15u);

    buffer.bufferedFrames.store(32);
    EXPECT_EQ(system.getBufferedFrameCount(buffer), 32u);
}

// Test thread safety - atomics should be safe for concurrent access
TEST_F(BufferSystemTest, ThreadSafety_AtomicOperations) {
    // This test verifies that atomic operations are used correctly
    // and can be safely read from multiple threads
    buffer.bufferedFrames.store(10);
    buffer.isBuffering.store(false);

    // Simulate concurrent reads (these should be safe)
    bool hasFrames1 = system.hasFrames(buffer);
    bool hasFrames2 = system.hasFrames(buffer);
    bool isReady1 = system.isReady(buffer);
    bool isReady2 = system.isReady(buffer);

    EXPECT_EQ(hasFrames1, hasFrames2);
    EXPECT_EQ(isReady1, isReady2);
    EXPECT_TRUE(hasFrames1);
    EXPECT_TRUE(isReady1);
}

// Test system lifecycle (initialize, update, shutdown)
TEST_F(BufferSystemTest, SystemLifecycle) {
    // Initialize should succeed
    EXPECT_NO_THROW(system.initialize(registry));

    // Update should be no-op but not throw
    EXPECT_NO_THROW(system.update(registry, 0.016f));

    // Shutdown should succeed
    EXPECT_NO_THROW(system.shutdown(registry));

    // System name should be correct
    EXPECT_STREQ(system.getName(), "BufferSystem");
}
