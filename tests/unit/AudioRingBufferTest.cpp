#include "entity/audio/AudioRingBuffer.hpp"
#include <gtest/gtest.h>
#include <vector>

using entity::AudioRingBuffer;

TEST(AudioRingBuffer, WriteThenReadRoundTrips) {
    AudioRingBuffer rb(8, 2);
    std::vector<float> in = {1,1, 2,2, 3,3};
    EXPECT_EQ(rb.write(in.data(), 3), 3u);
    EXPECT_EQ(rb.availableFrames(), 3u);
    std::vector<float> out(8, -1.0f);
    EXPECT_EQ(rb.read(out.data(), 4), 3u);          // 3 real, 1 zero-filled
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[4], 3.0f);
    EXPECT_FLOAT_EQ(out[6], 0.0f);                  // zero-filled shortfall
}

TEST(AudioRingBuffer, WriteSaturatesAtCapacity) {
    AudioRingBuffer rb(4, 2);
    std::vector<float> in(10 * 2, 1.0f);
    EXPECT_EQ(rb.write(in.data(), 10), 4u);         // capacity caps the write
}

TEST(AudioRingBuffer, WrapAround) {
    AudioRingBuffer rb(4, 1);
    std::vector<float> in = {1,2,3};
    std::vector<float> out(3);
    rb.write(in.data(), 3);
    rb.read(out.data(), 3);
    std::vector<float> in2 = {4,5,6};
    EXPECT_EQ(rb.write(in2.data(), 3), 3u);         // wraps past the end
    rb.read(out.data(), 3);
    EXPECT_FLOAT_EQ(out[0], 4.0f);
    EXPECT_FLOAT_EQ(out[2], 6.0f);
}
