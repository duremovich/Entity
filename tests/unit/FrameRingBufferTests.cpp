#include <gtest/gtest.h>
#include "entity/media/FrameRingBuffer.hpp"

using namespace entity;

class FrameRingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a small buffer for testing
        buffer = std::make_unique<FrameRingBuffer>(4);
    }

    void TearDown() override {
        buffer.reset();
    }

    // Helper to create a test frame
    DecodedFrame createFrame(FrameNumber frameNum, uint32_t width, uint32_t height) {
        DecodedFrame frame;
        frame.allocate(width, height);
        frame.frameNumber = frameNum;
        frame.pts = frameNum * 33333; // ~30fps
        frame.valid.store(true, std::memory_order_release);

        // Fill with test pattern (frame number repeated)
        uint8_t pattern = static_cast<uint8_t>(frameNum % 256);
        std::fill(frame.data.begin(), frame.data.end(), pattern);

        return frame;
    }

    std::unique_ptr<FrameRingBuffer> buffer;
};

// Test: Basic construction
TEST_F(FrameRingBufferTest, Construction) {
    EXPECT_EQ(buffer->getCapacity(), 4);
    EXPECT_EQ(buffer->getCount(), 0);
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_FALSE(buffer->isFull());
    EXPECT_FLOAT_EQ(buffer->getFillPercentage(), 0.0f);
}

// Test: Default capacity construction
TEST(FrameRingBufferStandaloneTest, DefaultCapacity) {
    FrameRingBuffer defaultBuffer;
    EXPECT_EQ(defaultBuffer.getCapacity(), FrameRingBuffer::DEFAULT_CAPACITY);
    EXPECT_EQ(defaultBuffer.getCount(), 0);
}

// Test: Push single frame
TEST_F(FrameRingBufferTest, PushSingleFrame) {
    auto frame = createFrame(0, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame)));
    EXPECT_EQ(buffer->getCount(), 1);
    EXPECT_FALSE(buffer->isEmpty());
    EXPECT_FALSE(buffer->isFull());
    EXPECT_FLOAT_EQ(buffer->getFillPercentage(), 0.25f);
}

// Test: Push multiple frames
TEST_F(FrameRingBufferTest, PushMultipleFrames) {
    for (int i = 0; i < 3; ++i) {
        auto frame = createFrame(i, 1920, 1080);
        EXPECT_TRUE(buffer->push(std::move(frame)));
    }
    EXPECT_EQ(buffer->getCount(), 3);
    EXPECT_FALSE(buffer->isEmpty());
    EXPECT_FALSE(buffer->isFull());
    EXPECT_FLOAT_EQ(buffer->getFillPercentage(), 0.75f);
}

// Test: Fill buffer to capacity
TEST_F(FrameRingBufferTest, FillToCapacity) {
    for (int i = 0; i < 4; ++i) {
        auto frame = createFrame(i, 1920, 1080);
        EXPECT_TRUE(buffer->push(std::move(frame)));
    }
    EXPECT_EQ(buffer->getCount(), 4);
    EXPECT_FALSE(buffer->isEmpty());
    EXPECT_TRUE(buffer->isFull());
    EXPECT_FLOAT_EQ(buffer->getFillPercentage(), 1.0f);
}

// Test: Push to full buffer fails
TEST_F(FrameRingBufferTest, PushToFullBufferFails) {
    // Fill buffer
    for (int i = 0; i < 4; ++i) {
        auto frame = createFrame(i, 1920, 1080);
        EXPECT_TRUE(buffer->push(std::move(frame)));
    }

    // Try to push one more
    auto extraFrame = createFrame(4, 1920, 1080);
    EXPECT_FALSE(buffer->push(std::move(extraFrame)));
    EXPECT_EQ(buffer->getCount(), 4);
    EXPECT_TRUE(buffer->isFull());
}

// Test: Pop from empty buffer fails
TEST_F(FrameRingBufferTest, PopFromEmptyBufferFails) {
    DecodedFrame frame;
    EXPECT_FALSE(buffer->pop(frame));
    EXPECT_TRUE(buffer->isEmpty());
}

// Test: Pop single frame
TEST_F(FrameRingBufferTest, PopSingleFrame) {
    auto frame = createFrame(0, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame)));

    DecodedFrame poppedFrame;
    EXPECT_TRUE(buffer->pop(poppedFrame));
    EXPECT_EQ(buffer->getCount(), 0);
    EXPECT_TRUE(buffer->isEmpty());

    // Verify popped frame data
    EXPECT_EQ(poppedFrame.frameNumber, 0);
    EXPECT_EQ(poppedFrame.width, 1920);
    EXPECT_EQ(poppedFrame.height, 1080);
    EXPECT_TRUE(poppedFrame.valid);
}

// Test: Push and pop multiple frames (FIFO order)
TEST_F(FrameRingBufferTest, PushPopFIFOOrder) {
    // Push frames 0, 1, 2
    for (int i = 0; i < 3; ++i) {
        auto frame = createFrame(i, 1920, 1080);
        EXPECT_TRUE(buffer->push(std::move(frame)));
    }

    // Pop and verify order
    for (int i = 0; i < 3; ++i) {
        DecodedFrame frame;
        EXPECT_TRUE(buffer->pop(frame));
        EXPECT_EQ(frame.frameNumber, i);
        EXPECT_EQ(buffer->getCount(), 2 - i);
    }

    EXPECT_TRUE(buffer->isEmpty());
}

// Test: Circular buffer wrapping
TEST_F(FrameRingBufferTest, CircularWrapping) {
    // Fill buffer
    for (int i = 0; i < 4; ++i) {
        auto frame = createFrame(i, 1920, 1080);
        EXPECT_TRUE(buffer->push(std::move(frame)));
    }

    // Pop 2 frames
    DecodedFrame frame;
    EXPECT_TRUE(buffer->pop(frame));
    EXPECT_EQ(frame.frameNumber, 0);
    EXPECT_TRUE(buffer->pop(frame));
    EXPECT_EQ(frame.frameNumber, 1);
    EXPECT_EQ(buffer->getCount(), 2);

    // Push 2 more frames (should wrap around)
    auto frame4 = createFrame(4, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame4)));
    auto frame5 = createFrame(5, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame5)));
    EXPECT_EQ(buffer->getCount(), 4);
    EXPECT_TRUE(buffer->isFull());

    // Pop all and verify order: 2, 3, 4, 5
    int expectedFrames[] = {2, 3, 4, 5};
    for (int i = 0; i < 4; ++i) {
        DecodedFrame f;
        EXPECT_TRUE(buffer->pop(f));
        EXPECT_EQ(f.frameNumber, expectedFrames[i]);
    }
    EXPECT_TRUE(buffer->isEmpty());
}

// Test: Peek without removing
TEST_F(FrameRingBufferTest, PeekWithoutRemoving) {
    auto frame = createFrame(42, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame)));

    // Peek at frame
    DecodedFrame peekedFrame;
    EXPECT_TRUE(buffer->peek(peekedFrame));
    EXPECT_EQ(peekedFrame.frameNumber, 42);
    EXPECT_EQ(peekedFrame.width, 1920);
    EXPECT_EQ(peekedFrame.height, 1080);

    // Buffer should still contain the frame
    EXPECT_EQ(buffer->getCount(), 1);
    EXPECT_FALSE(buffer->isEmpty());

    // Can still pop it
    DecodedFrame poppedFrame;
    EXPECT_TRUE(buffer->pop(poppedFrame));
    EXPECT_EQ(poppedFrame.frameNumber, 42);
    EXPECT_TRUE(buffer->isEmpty());
}

// Test: Peek from empty buffer fails
TEST_F(FrameRingBufferTest, PeekFromEmptyBufferFails) {
    DecodedFrame frame;
    EXPECT_FALSE(buffer->peek(frame));
}

// Test: Get frame by frame number
TEST_F(FrameRingBufferTest, GetFrameByNumber) {
    // Push frames 10, 20, 30
    auto frame10 = createFrame(10, 1920, 1080);
    auto frame20 = createFrame(20, 1920, 1080);
    auto frame30 = createFrame(30, 1920, 1080);

    EXPECT_TRUE(buffer->push(std::move(frame10)));
    EXPECT_TRUE(buffer->push(std::move(frame20)));
    EXPECT_TRUE(buffer->push(std::move(frame30)));

    // Get frame 20
    DecodedFrame frame;
    EXPECT_TRUE(buffer->getFrame(20, frame));
    EXPECT_EQ(frame.frameNumber, 20);
    EXPECT_EQ(frame.width, 1920);
    EXPECT_EQ(frame.height, 1080);

    // Buffer should still have all frames
    EXPECT_EQ(buffer->getCount(), 3);
}

// Test: Get frame not in buffer
TEST_F(FrameRingBufferTest, GetFrameNotFound) {
    auto frame = createFrame(10, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame)));

    DecodedFrame outFrame;
    EXPECT_FALSE(buffer->getFrame(999, outFrame));
}

// Test: Clear buffer
TEST_F(FrameRingBufferTest, ClearBuffer) {
    // Fill buffer
    for (int i = 0; i < 4; ++i) {
        auto frame = createFrame(i, 1920, 1080);
        EXPECT_TRUE(buffer->push(std::move(frame)));
    }
    EXPECT_TRUE(buffer->isFull());

    // Clear
    buffer->clear();
    EXPECT_TRUE(buffer->isEmpty());
    EXPECT_EQ(buffer->getCount(), 0);
    EXPECT_FALSE(buffer->isFull());

    // Should be able to push again
    auto frame = createFrame(100, 1920, 1080);
    EXPECT_TRUE(buffer->push(std::move(frame)));
    EXPECT_EQ(buffer->getCount(), 1);
}

// Test: DecodedFrame allocation
TEST(DecodedFrameTest, Allocation) {
    DecodedFrame frame;
    EXPECT_EQ(frame.width, 0);
    EXPECT_EQ(frame.height, 0);
    EXPECT_TRUE(frame.data.empty());
    EXPECT_FALSE(frame.valid.load(std::memory_order_acquire));

    frame.allocate(1920, 1080);
    EXPECT_EQ(frame.width, 1920);
    EXPECT_EQ(frame.height, 1080);
    EXPECT_EQ(frame.data.size(), 1920 * 1080 * 4); // RGBA
}

// Test: DecodedFrame clear
TEST(DecodedFrameTest, Clear) {
    DecodedFrame frame;
    frame.allocate(1920, 1080);
    frame.frameNumber = 42;
    frame.pts = 123456;
    frame.valid.store(true, std::memory_order_release);

    frame.clear();
    EXPECT_TRUE(frame.data.empty());
    EXPECT_EQ(frame.frameNumber, -1);
    EXPECT_EQ(frame.width, 0);
    EXPECT_EQ(frame.height, 0);
    EXPECT_EQ(frame.pts, 0);
    EXPECT_FALSE(frame.valid.load(std::memory_order_acquire));
}

// Test: Large capacity buffer
TEST(FrameRingBufferStandaloneTest, LargeCapacity) {
    FrameRingBuffer largeBuffer(100);
    EXPECT_EQ(largeBuffer.getCapacity(), 100);

    // Push 50 frames
    for (int i = 0; i < 50; ++i) {
        DecodedFrame frame;
        frame.allocate(100, 100);
        frame.frameNumber = i;
        frame.valid.store(true, std::memory_order_release);
        EXPECT_TRUE(largeBuffer.push(std::move(frame)));
    }

    EXPECT_EQ(largeBuffer.getCount(), 50);
    EXPECT_FLOAT_EQ(largeBuffer.getFillPercentage(), 0.5f);
}
