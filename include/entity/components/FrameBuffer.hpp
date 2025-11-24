#pragma once

#include "../core/Types.hpp"
#include <atomic>
#include <memory>

// Forward declaration
namespace entity { class FrameRingBuffer; }

namespace entity {

/**
 * FrameBuffer component for decoded frame buffering.
 *
 * Contains a circular ring buffer of decoded frames and tracks
 * the current playback position (PTS - Presentation TimeStamp).
 *
 * PURE DATA COMPONENT - No methods, only data members.
 * Logic for buffer management extracted to BufferSystem.
 */
struct FrameBuffer {
    // Shared pointer to ring buffer (may be shared between components)
    std::shared_ptr<FrameRingBuffer> ringBuffer;

    // Current presentation timestamp (in microseconds)
    std::atomic<Timestamp> currentPTS{0};

    // Target frame for decode-ahead
    std::atomic<FrameNumber> targetFrame{0};

    // Buffer state (atomic for thread-safe access from decode threads)
    std::atomic<bool> isBuffering{true};   // True if still filling buffer
    std::atomic<uint32_t> bufferedFrames{0};  // Number of frames in buffer
};

} // namespace entity
