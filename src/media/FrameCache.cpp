#include "entity/media/FrameCache.hpp"

namespace entity {

FrameCache::FrameCache(size_t maxBytes)
    : m_maxBytes(maxBytes)
{
}

FrameCache::~FrameCache() = default;

void FrameCache::put(entt::entity clip, FrameNumber frame, DecodedFrame&& data) {
    // Disabled cache: don't pretend to store anything. Caller should still
    // function (next get() returns a miss, decoder gets re-asked).
    if (m_maxBytes == 0) return;

    const size_t bytes = data.data.size();
    // A zero-byte frame is almost certainly a decode error; refuse to cache
    // it so a bad decode doesn't pollute the cache with phantom hits that
    // upload empty buffers.
    if (bytes == 0) return;

    // Move into a heap allocation so the cache can hand out shared_ptr<const>
    // aliases without copying the pixel buffer.
    auto framePtr = std::make_shared<DecodedFrame>(std::move(data));

    std::lock_guard<std::mutex> lock(m_mutex);

    const Key key{clip, frame};

    // Replace-in-place if the key already exists. Common when a clip resyncs
    // from the same source position (e.g. ping-pong overshoot then re-seek).
    if (auto it = m_index.find(key); it != m_index.end()) {
        m_bytesUsed -= it->second->bytes;
        m_lru.erase(it->second);
        m_index.erase(it);
    }

    m_lru.push_front(Entry{key, std::shared_ptr<const DecodedFrame>(std::move(framePtr)), bytes});
    m_index.emplace(key, m_lru.begin());
    m_bytesUsed += bytes;

    evictUntilUnderBudget();
}

FrameLease FrameCache::get(entt::entity clip, FrameNumber frame) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_index.find(Key{clip, frame});
    if (it == m_index.end()) {
        return FrameLease{};
    }

    // Promote to MRU (front of list). std::list::splice keeps the iterator
    // valid, so the index entry doesn't need updating.
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return FrameLease{it->second->frame};
}

bool FrameCache::has(entt::entity clip, FrameNumber frame) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.find(Key{clip, frame}) != m_index.end();
}

std::optional<FrameNumber> FrameCache::nearestTo(entt::entity clip, FrameNumber target) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Linear scan over all entries belonging to this clip. With LRU storage
    // we don't keep a per-clip sorted index; under steady-state playback
    // the cache holds tens-to-hundreds of entries total, so this is fine.
    // If profiling ever flags it, the fix is a per-clip sorted set of frame
    // numbers maintained alongside the LRU list.
    std::optional<FrameNumber> best;
    int64_t                    bestDist = 0;

    for (const auto& entry : m_lru) {
        if (entry.key.clip != clip) continue;
        int64_t dist = static_cast<int64_t>(entry.key.frame) - static_cast<int64_t>(target);
        if (dist < 0) dist = -dist;
        if (!best || dist < bestDist) {
            best = entry.key.frame;
            bestDist = dist;
        }
    }
    return best;
}

void FrameCache::evictClip(entt::entity clip) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_lru.begin(); it != m_lru.end(); ) {
        if (it->key.clip == clip) {
            m_bytesUsed -= it->bytes;
            m_index.erase(it->key);
            it = m_lru.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameCache::setMaxBytes(size_t maxBytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxBytes = maxBytes;
    evictUntilUnderBudget();
}

size_t FrameCache::maxBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxBytes;
}

size_t FrameCache::bytesUsed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytesUsed;
}

size_t FrameCache::entryCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.size();
}

void FrameCache::evictUntilUnderBudget() {
    // Caller holds m_mutex.
    while (m_bytesUsed > m_maxBytes && !m_lru.empty()) {
        auto& tail = m_lru.back();
        m_bytesUsed -= tail.bytes;
        m_index.erase(tail.key);
        m_lru.pop_back();
    }
}

} // namespace entity
