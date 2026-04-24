#include "entity/media/FrameRingBuffer.hpp"
#include <algorithm>
#include <cstdlib>
#include <climits>

namespace entity {

FrameRingBuffer::FrameRingBuffer(uint32_t capacity)
    : m_capacity(capacity)
{
    m_frames.resize(capacity);
}

bool FrameRingBuffer::push(DecodedFrame&& frame) {
    // Check if buffer is full
    if (isFull()) {
        return false;
    }

    // Get write index
    uint32_t writeIdx = m_writeIndex.load(std::memory_order_acquire);

    // Move frame into buffer
    m_frames[writeIdx] = std::move(frame);

    // Update write index (wrap around)
    uint32_t nextWriteIdx = (writeIdx + 1) % m_capacity;
    m_writeIndex.store(nextWriteIdx, std::memory_order_release);

    // Increment count
    m_count.fetch_add(1, std::memory_order_release);

    return true;
}

bool FrameRingBuffer::pop(DecodedFrame& outFrame) {
    // Check if buffer is empty
    if (isEmpty()) {
        return false;
    }

    // Get read index
    uint32_t readIdx = m_readIndex.load(std::memory_order_acquire);

    // Move frame out of buffer
    outFrame = std::move(m_frames[readIdx]);

    // Clear the slot
    m_frames[readIdx].clear();

    // Update read index (wrap around)
    uint32_t nextReadIdx = (readIdx + 1) % m_capacity;
    m_readIndex.store(nextReadIdx, std::memory_order_release);

    // Decrement count
    m_count.fetch_sub(1, std::memory_order_release);

    return true;
}

bool FrameRingBuffer::peek(DecodedFrame& outFrame) const {
    // Check if buffer is empty
    if (isEmpty()) {
        return false;
    }

    // Get read index
    uint32_t readIdx = m_readIndex.load(std::memory_order_acquire);

    // Copy frame (don't move, as we're peeking)
    outFrame = m_frames[readIdx];

    return true;
}

bool FrameRingBuffer::getFrame(FrameNumber frameNumber, DecodedFrame& outFrame) const {
    // Get current count and read index
    uint32_t count = m_count.load(std::memory_order_acquire);

    // SAFETY: Check for corrupted count (race condition with clear())
    if (count == 0 || count > m_capacity) {
        return false;
    }

    uint32_t readIdx = m_readIndex.load(std::memory_order_acquire);

    // Search through buffered frames
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (readIdx + i) % m_capacity;
        const DecodedFrame& frame = m_frames[idx];

        if (frame.valid.load(std::memory_order_acquire) && frame.frameNumber == frameNumber) {
            outFrame = frame; // Copy the frame
            return true;
        }
    }

    return false;
}

bool FrameRingBuffer::consumeUpTo(FrameNumber frameNumber, DecodedFrame& outFrame) {
    // Get current count and read index
    uint32_t count = m_count.load(std::memory_order_acquire);
    if (count == 0) {
        return false; // Buffer empty
    }

    // SAFETY: Check for corrupted count (race condition with clear())
    // If count is larger than capacity, buffer was cleared during a race - bail out
    if (count > m_capacity) {
        m_count.store(0, std::memory_order_release);
        return false;
    }

    uint32_t readIdx = m_readIndex.load(std::memory_order_acquire);

    // Find the best frame to return:
    // 1. If exact frame found, use it
    // 2. Otherwise, use the NEWEST frame with frameNumber <= target (drain stale frames)
    // 3. Last resort: if all buffered frames are past target, use oldest
    int32_t foundOffset      = -1;
    int32_t newestBelowOff   = -1;
    FrameNumber newestBelowNum = 0;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (readIdx + i) % m_capacity;
        const DecodedFrame& frame = m_frames[idx];

        if (!frame.valid.load(std::memory_order_acquire)) continue;

        if (frame.frameNumber == frameNumber) {
            foundOffset = static_cast<int32_t>(i);
            break;
        }
        if (frame.frameNumber < frameNumber &&
            (newestBelowOff < 0 || frame.frameNumber > newestBelowNum)) {
            newestBelowOff = static_cast<int32_t>(i);
            newestBelowNum = frame.frameNumber;
        }
    }

    int32_t bestOffset = 0;
    if (foundOffset >= 0) {
        bestOffset = foundOffset;
    } else if (newestBelowOff >= 0) {
        // Decoder is behind - return the closest-to-target frame we have, drop everything older
        bestOffset = newestBelowOff;
    } else {
        // All frames are past target (unusual) - drop the oldest and retry next tick
        bestOffset = 0;
    }

    // Copy the frame - check validity with atomic load to prevent TOCTOU race
    uint32_t frameIdx = (readIdx + static_cast<uint32_t>(bestOffset)) % m_capacity;
    const DecodedFrame& selectedFrame = m_frames[frameIdx];
    if (!selectedFrame.valid.load(std::memory_order_acquire)) {
        return false; // No valid frame
    }
    outFrame = selectedFrame;

    // Pop all frames from head up to and including the selected frame
    uint32_t framesToConsume = static_cast<uint32_t>(bestOffset) + 1;

    for (uint32_t i = 0; i < framesToConsume; ++i) {
        // SAFETY: Check count before decrementing to prevent wraparound race with clear()
        uint32_t currentCount = m_count.load(std::memory_order_acquire);
        if (currentCount == 0 || currentCount > m_capacity) {
            // Buffer was cleared by another thread during our operation - stop
            break;
        }

        uint32_t idx = m_readIndex.load(std::memory_order_acquire);
        m_frames[idx].clear();

        uint32_t nextReadIdx = (idx + 1) % m_capacity;
        m_readIndex.store(nextReadIdx, std::memory_order_release);
        m_count.fetch_sub(1, std::memory_order_release);
    }

    return true;
}

bool FrameRingBuffer::getNearestFrame(FrameNumber targetFrame, DecodedFrame& outFrame) const {
    // Get current count and read index
    uint32_t count = m_count.load(std::memory_order_acquire);

    // SAFETY: Check for corrupted count (race condition with clear())
    if (count == 0 || count > m_capacity) {
        return false; // Buffer empty or corrupted
    }

    uint32_t readIdx = m_readIndex.load(std::memory_order_acquire);

    // First, try to find the exact frame
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (readIdx + i) % m_capacity;
        const DecodedFrame& frame = m_frames[idx];

        if (frame.valid.load(std::memory_order_acquire) && frame.frameNumber == targetFrame) {
            outFrame = frame;
            return true;
        }
    }

    // Exact frame not found - return the closest valid frame
    // Prefer frames close to target, but any frame is better than freezing
    int32_t bestDistance = INT32_MAX;
    int32_t bestOffset = -1;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (readIdx + i) % m_capacity;
        const DecodedFrame& frame = m_frames[idx];

        if (frame.valid.load(std::memory_order_acquire)) {
            int32_t distance = std::abs(static_cast<int32_t>(frame.frameNumber) - static_cast<int32_t>(targetFrame));
            if (distance < bestDistance) {
                bestDistance = distance;
                bestOffset = static_cast<int32_t>(i);
            }
        }
    }

    if (bestOffset >= 0) {
        uint32_t idx = (readIdx + static_cast<uint32_t>(bestOffset)) % m_capacity;
        outFrame = m_frames[idx];
        return true;
    }

    return false; // No valid frame found
}

void FrameRingBuffer::clear() {
    // IMPORTANT: Set count to 0 FIRST to prevent data race
    // Other threads check count before accessing frames, so this ensures
    // they see empty buffer before we start clearing frame data
    m_count.store(0, std::memory_order_seq_cst);

    // Reset indices
    m_writeIndex.store(0, std::memory_order_release);
    m_readIndex.store(0, std::memory_order_release);

    // Now safe to clear frame data - other threads won't access with count=0
    for (auto& frame : m_frames) {
        frame.clear();
    }
}

} // namespace entity
