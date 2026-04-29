#include "entity/media/DecodeBufferPool.hpp"

namespace entity {

DecodeBufferPool::DecodeBufferPool(std::size_t maxBuffersPerSize,
                                   std::size_t maxTotal)
    : m_maxBuffersPerSize(maxBuffersPerSize)
    , m_maxTotal(maxTotal)
{}

std::vector<std::uint8_t> DecodeBufferPool::acquire(std::size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_buckets.find(bytes);
        if (it != m_buckets.end() && !it->second.empty()) {
            std::vector<std::uint8_t> buf = std::move(it->second.back());
            it->second.pop_back();
            --m_totalEntries;
            // The previous owner's bytes are still in the buffer; the
            // decoder writes over them, so we don't zero-init. size()
            // is preserved through std::move + the original alloc.
            return buf;
        }
    }
    // Miss -- fresh allocation. value-initialized to 0 (one-time cold-
    // start cost; subsequent recycles avoid it).
    return std::vector<std::uint8_t>(bytes);
}

void DecodeBufferPool::release(std::vector<std::uint8_t>&& buf) {
    if (buf.empty()) return;  // Nothing useful to recycle.

    const std::size_t bytes = buf.size();

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_totalEntries >= m_maxTotal) {
        // Global cap reached; let the buffer free as the rvalue goes
        // out of scope at function exit.
        return;
    }

    auto& bucket = m_buckets[bytes];
    if (bucket.size() >= m_maxBuffersPerSize) {
        // Bucket cap reached. Same drop policy as the global cap.
        return;
    }

    bucket.push_back(std::move(buf));
    ++m_totalEntries;
}

std::size_t DecodeBufferPool::entries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalEntries;
}

std::size_t DecodeBufferPool::bucketCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buckets.size();
}

} // namespace entity
