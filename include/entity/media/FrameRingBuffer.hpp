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
    std::vector<uint8_t> data;    // Pixel data OR pre-compressed BC blocks (see `format`)
    FrameNumber frameNumber{-1};   // Frame number in media timeline
    uint32_t width{0};
    uint32_t height{0};
    Timestamp pts{0};              // Presentation timestamp (microseconds)
    TextureFormat format{TextureFormat::RGBA8_UNORM}; // How to interpret `data` on upload
    std::atomic<bool> valid{false}; // True if frame contains valid data (atomic for thread safety)

    // Default constructor
    DecodedFrame() = default;

    // Copy constructor - atomics must be copied explicitly
    DecodedFrame(const DecodedFrame& other)
        : data(other.data)
        , frameNumber(other.frameNumber)
        , width(other.width)
        , height(other.height)
        , pts(other.pts)
        , format(other.format)
        , valid(other.valid.load(std::memory_order_acquire))
    {}

    // Copy assignment operator - atomics must be copied explicitly
    DecodedFrame& operator=(const DecodedFrame& other) {
        if (this != &other) {
            data = other.data;
            frameNumber = other.frameNumber;
            width = other.width;
            height = other.height;
            pts = other.pts;
            format = other.format;
            valid.store(other.valid.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    // Move constructor
    DecodedFrame(DecodedFrame&& other) noexcept
        : data(std::move(other.data))
        , frameNumber(other.frameNumber)
        , width(other.width)
        , height(other.height)
        , pts(other.pts)
        , format(other.format)
        , valid(other.valid.load(std::memory_order_acquire))
    {}

    // Move assignment operator
    DecodedFrame& operator=(DecodedFrame&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            frameNumber = other.frameNumber;
            width = other.width;
            height = other.height;
            pts = other.pts;
            format = other.format;
            valid.store(other.valid.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    /**
     * Bytes required for a frame of (w, h) in the given format.
     * RGBA8 → w*h*4. BC formats use 4x4 blocks (8 or 16 bytes per block).
     */
    static size_t bytesFor(uint32_t w, uint32_t h, TextureFormat fmt) {
        switch (fmt) {
            case TextureFormat::RGBA8_UNORM:
                return static_cast<size_t>(w) * h * 4;
            case TextureFormat::BC1_UNORM:
            case TextureFormat::BC4_UNORM: {
                const size_t bx = (w + 3) / 4;
                const size_t by = (h + 3) / 4;
                return bx * by * 8;
            }
            case TextureFormat::BC3_UNORM:
            case TextureFormat::BC6H_UF16:
            case TextureFormat::BC6H_SF16:
            case TextureFormat::BC7_UNORM: {
                const size_t bx = (w + 3) / 4;
                const size_t by = (h + 3) / 4;
                return bx * by * 16;
            }
        }
        return 0;
    }

    /**
     * Allocate memory for pixel data. Defaults to RGBA8 sizing for
     * backward compatibility with the ProRes / PNG / H.264 CPU paths;
     * HAP callers pass a BC format and get block-packed sizing.
     */
    void allocate(uint32_t w, uint32_t h, TextureFormat fmt = TextureFormat::RGBA8_UNORM) {
        width = w;
        height = h;
        format = fmt;
        data.resize(bytesFor(w, h, fmt));
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
        format = TextureFormat::RGBA8_UNORM;
        valid.store(false, std::memory_order_release);
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
    static constexpr uint32_t DEFAULT_CAPACITY = 64; // ~2s @ 30fps / ~1s @ 60fps; bumped from 32 for decode-dip headroom

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
     * Consume frames up to and including the specified frame number.
     * Searches the buffer for the frame, pops all frames from head up to it,
     * and returns the requested frame. This keeps the buffer flowing during playback.
     *
     * @param frameNumber Frame number to consume up to
     * @param outFrame Frame to receive data if found
     * @return true if frame found and consumed, false otherwise
     */
    bool consumeUpTo(FrameNumber frameNumber, DecodedFrame& outFrame);

    /**
     * Get the nearest available frame to the target frame number.
     * If exact frame isn't found, returns any frame in the buffer.
     * This prevents visual freezing when decode thread is catching up.
     *
     * @param targetFrame Frame number we're looking for
     * @param outFrame Frame to receive data if any frame is found
     * @return true if any frame is available, false if buffer is empty
     */
    bool getNearestFrame(FrameNumber targetFrame, DecodedFrame& outFrame) const;

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
