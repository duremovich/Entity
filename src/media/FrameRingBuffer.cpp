#include "entity/media/FrameRingBuffer.hpp"
#include <algorithm>

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
    uint32_t readIdx = m_readIndex.load(std::memory_order_acquire);

    // Search through buffered frames
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (readIdx + i) % m_capacity;
        const DecodedFrame& frame = m_frames[idx];

        if (frame.valid && frame.frameNumber == frameNumber) {
            outFrame = frame; // Copy the frame
            return true;
        }
    }

    return false;
}

void FrameRingBuffer::clear() {
    // Clear all frames
    for (auto& frame : m_frames) {
        frame.clear();
    }

    // Reset indices and count
    m_writeIndex.store(0, std::memory_order_release);
    m_readIndex.store(0, std::memory_order_release);
    m_count.store(0, std::memory_order_release);
}

} // namespace entity
