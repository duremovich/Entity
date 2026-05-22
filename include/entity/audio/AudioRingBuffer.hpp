#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace entity {

// Single-producer (audio decode worker) / single-consumer (mixer)
// lock-free FIFO of interleaved float32 audio frames. One "frame" is
// `channels` floats. Capacity is fixed at construction.
class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacityFrames, int channels)
        : m_channels(channels),
          m_capacity(capacityFrames + 1),            // one slot reserved
          m_data((capacityFrames + 1) * channels, 0.0f) {}

    int channels() const { return m_channels; }

    // Producer side. Writes up to `frames`; returns frames actually
    // written (short when the buffer is near-full).
    size_t write(const float* interleaved, size_t frames) {
        const size_t w = m_write.load(std::memory_order_relaxed);
        const size_t r = m_read.load(std::memory_order_acquire);
        const size_t freeFrames = (r + m_capacity - w - 1) % m_capacity;
        const size_t n = frames < freeFrames ? frames : freeFrames;
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = ((w + i) % m_capacity) * m_channels;
            std::memcpy(&m_data[slot], &interleaved[i * m_channels],
                        m_channels * sizeof(float));
        }
        m_write.store((w + n) % m_capacity, std::memory_order_release);
        return n;
    }

    // Consumer side. Reads up to `frames` into `out`; ZERO-FILLS any
    // shortfall so the mixer always gets a full block. Returns the count
    // of real (non-zero-filled) frames.
    size_t read(float* out, size_t frames) {
        const size_t r = m_read.load(std::memory_order_relaxed);
        const size_t w = m_write.load(std::memory_order_acquire);
        const size_t avail = (w + m_capacity - r) % m_capacity;
        const size_t n = frames < avail ? frames : avail;
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = ((r + i) % m_capacity) * m_channels;
            std::memcpy(&out[i * m_channels], &m_data[slot],
                        m_channels * sizeof(float));
        }
        if (n < frames) {
            std::memset(&out[n * m_channels], 0,
                        (frames - n) * m_channels * sizeof(float));
        }
        m_read.store((r + n) % m_capacity, std::memory_order_release);
        return n;
    }

    size_t availableFrames() const {
        const size_t r = m_read.load(std::memory_order_acquire);
        const size_t w = m_write.load(std::memory_order_acquire);
        return (w + m_capacity - r) % m_capacity;
    }

    // Producer-side reset. Only call when the consumer is known idle
    // (worker flushing on a seek before it resumes; mixer source inactive).
    void clear() {
        m_read.store(0, std::memory_order_relaxed);
        m_write.store(0, std::memory_order_relaxed);
    }

private:
    const int           m_channels;
    const size_t        m_capacity;
    std::vector<float>  m_data;
    std::atomic<size_t> m_write{0};
    std::atomic<size_t> m_read{0};
};

} // namespace entity
