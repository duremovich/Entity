#pragma once

#include "entity/core/Types.hpp"
#include <vector>
#include <atomic>
#include <memory>
#include <cstdint>

namespace entity {

/**
 * DecodedFrame - Single decoded video frame with metadata.
 *
 * Contains raw RGBA pixel data and frame information.
 */
struct DecodedFrame {
    std::vector<uint8_t> data;    // Raw RGBA pixel data (premultiplied alpha)
    FrameNumber frameNumber{-1};   // Frame number in media timeline
    uint32_t width{0};
    uint32_t height{0};
    Timestamp pts{0};              // Presentation timestamp (microseconds)
    bool valid{false};             // True if frame contains valid data

    /**
     * Allocate memory for pixel data.
     */
    void allocate(uint32_t w, uint32_t h) {
        width = w;
        height = h;
        data.resize(static_cast<size_t>(width) * height * 4); // RGBA = 4 bytes per pixel
    }

    /**
     * Clear frame data and mark as invalid.
     */
    void clear() {
        data.clear();
        frameNumber = -1;
        width = 0;
        height = 0;
        pts = 0;
        valid = false;
    }
};

/**
 * FrameRingBuffer - Lock-free circular buffer for decoded frames.
 *
 * Thread-safe ring buffer using atomic operations. Designed for
 * single producer (decode thread) / single consumer (render thread) pattern.
 *
 * Features:
 * - Lock-free push/pop operations
 * - Fixed capacity (default 32 frames)
 * - O(1) operations
 * - No blocking on full/empty conditions
 */
class FrameRingBuffer {
public:
    static constexpr uint32_t DEFAULT_CAPACITY = 32; // ~1 second @ 30fps

    /**
     * Construct ring buffer with specified capacity.
     */
    explicit FrameRingBuffer(uint32_t capacity = DEFAULT_CAPACITY);

    /**
     * Push a frame into the buffer.
     *
     * @param frame Frame to push (will be moved)
     * @return true if successful, false if buffer is full
     */
    bool push(DecodedFrame&& frame);

    /**
     * Pop a frame from the buffer.
     *
     * @param outFrame Frame to receive popped data
     * @return true if successful, false if buffer is empty
     */
    bool pop(DecodedFrame& outFrame);

    /**
     * Peek at the next frame without removing it.
     *
     * @param outFrame Frame to receive peeked data (copied, not moved)
     * @return true if successful, false if buffer is empty
     */
    bool peek(DecodedFrame& outFrame) const;

    /**
     * Get frame by frame number without removing it.
     * Searches the buffer for a specific frame number.
     *
     * @param frameNumber Frame number to find
     * @param outFrame Frame to receive data if found
     * @return true if frame found, false otherwise
     */
    bool getFrame(FrameNumber frameNumber, DecodedFrame& outFrame) const;

    /**
     * Clear all frames from the buffer.
     */
    void clear();

    /**
     * Check if buffer is empty.
     */
    bool isEmpty() const {
        return m_count.load(std::memory_order_acquire) == 0;
    }

    /**
     * Check if buffer is full.
     */
    bool isFull() const {
        return m_count.load(std::memory_order_acquire) >= m_capacity;
    }

    /**
     * Get number of frames currently in buffer.
     */
    uint32_t getCount() const {
        return m_count.load(std::memory_order_acquire);
    }

    /**
     * Get buffer capacity.
     */
    uint32_t getCapacity() const {
        return m_capacity;
    }

    /**
     * Get buffer fill percentage (0.0 to 1.0).
     */
    float getFillPercentage() const {
        return static_cast<float>(getCount()) / static_cast<float>(m_capacity);
    }

private:
    std::vector<DecodedFrame> m_frames;  // Circular buffer storage
    uint32_t m_capacity;                  // Maximum number of frames

    std::atomic<uint32_t> m_writeIndex{0}; // Next index to write to
    std::atomic<uint32_t> m_readIndex{0};  // Next index to read from
    std::atomic<uint32_t> m_count{0};      // Number of frames in buffer
};

} // namespace entity
