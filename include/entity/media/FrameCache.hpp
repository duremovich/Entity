#pragma once

#include "entity/core/Types.hpp"
#include "entity/media/DecodedFrame.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace entity {

/**
 * FrameCache — sparse LRU cache of decoded frames keyed by (clip entity,
 * source frame number). Replaces the per-clip FrameRingBuffer.
 *
 * Why this instead of a per-clip ring:
 *   - Click-to-seek-into-recently-viewed-territory is O(1) (cache hit, no
 *     re-decode), instead of "buffer was consumed past it, re-decode from
 *     keyframe".
 *   - Ping-pong's bidirectional access "just works" — frames are not
 *     consumed by readers. As long as the per-clip working set fits under
 *     the budget, a ping-pong loop never re-decodes.
 *   - Memory budget is global, not per-clip — eight idle clips don't
 *     each pin 64-frame ring buffers.
 *
 * Threading: single mutex over the LRU list + index map. Producer (decode
 * thread per clip) and consumer (main thread) traffic is modest; if profiling
 * shows the lock as a bottleneck, the obvious next step is sharding by
 * clip-entity hash. Frames themselves are stored as `shared_ptr<const
 * DecodedFrame>`, so reads return cheap aliasing pointers and outstanding
 * `FrameLease` holders pin the frame's memory through eviction (the cache
 * forgets it; the bytes survive until the last lease drops). This keeps
 * the GPU-upload path safe — it does not need to hold the cache mutex while
 * memcpy'ing the pixel data.
 *
 * Bytes accounting reflects what is currently held by the cache map, not
 * outstanding leases. Net effect: with N readers each holding a ~33 MB
 * frame and the cache evicting them, real RSS briefly overshoots the
 * budget by ~N×33 MB until those leases drop. Under sane usage this is
 * one or two frames, not a problem.
 */
class FrameLease;

class FrameCache {
public:
    /**
     * @param maxBytes Initial RAM budget (use Settings::frameCacheBytes).
     *                 0 disables the cache (every put is a no-op, every get
     *                 misses) — useful for diagnostics, not normal operation.
     */
    explicit FrameCache(size_t maxBytes);
    ~FrameCache();

    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    /**
     * Insert a freshly-decoded frame for (clip, frame). If an entry for that
     * key already exists, it is replaced (the new frame's bytes count, the
     * old entry is evicted). Eviction of LRU entries runs after insert until
     * total bytes ≤ maxBytes.
     *
     * Decode threads call this. Move-only — the cache takes ownership of
     * `data`'s pixel buffer.
     */
    void put(entt::entity clip, FrameNumber frame, DecodedFrame&& data);

    /**
     * Look up an exact (clip, frame) match. Returns an invalid lease on miss.
     * On hit, the lease pins the frame so concurrent eviction cannot free
     * the underlying memory while the caller is using it.
     *
     * Touching an entry promotes it to MRU.
     */
    FrameLease get(entt::entity clip, FrameNumber frame);

    /** Cheap "is it cached" check. Does not touch LRU order. */
    bool has(entt::entity clip, FrameNumber frame) const;

    /**
     * Find the cached frame for `clip` whose number is closest to `target`.
     * Used by PlaybackController to fall back to "nearest available" while
     * the decoder is still catching up during forward playback.
     *
     * Read-only — does not promote LRU.
     */
    std::optional<FrameNumber> nearestTo(entt::entity clip, FrameNumber target) const;

    /**
     * Drop every cached frame for `clip`. Call when the clip is removed
     * from the scene, or when the decoder is about to seek to a position
     * far from anything currently cached and you want to free that memory
     * proactively. (For routine seeks within a clip, eviction will happen
     * naturally as new frames push old ones out.)
     */
    void evictClip(entt::entity clip);

    /**
     * Update the budget at runtime (Settings dialog "Apply"). Shrinks the
     * cache by evicting LRU entries until under the new limit. Growing the
     * budget just allows future puts to keep more entries; nothing happens
     * immediately.
     */
    void setMaxBytes(size_t maxBytes);

    // Diagnostics — for the perf overlay + tests.
    size_t maxBytes() const;
    size_t bytesUsed() const;
    size_t entryCount() const;

private:
    struct Key {
        entt::entity clip;
        FrameNumber  frame;
        bool operator==(const Key& other) const noexcept {
            return clip == other.clip && frame == other.frame;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            // entt::entity is just a uint32_t under the hood; FrameNumber is
            // 64-bit. Mix them — basic hash combine is fine, this isn't
            // adversarial.
            const auto entId = static_cast<uint64_t>(static_cast<uint32_t>(k.clip));
            const auto fr    = static_cast<uint64_t>(k.frame);
            return std::hash<uint64_t>{}(entId * 0x9E3779B97F4A7C15ull ^ fr);
        }
    };

    struct Entry {
        Key                                key;
        std::shared_ptr<const DecodedFrame> frame;
        size_t                             bytes;
    };

    using Lru   = std::list<Entry>;
    using Index = std::unordered_map<Key, Lru::iterator, KeyHash>;

    // Evict from LRU tail until bytes fit. Caller must hold m_mutex.
    void evictUntilUnderBudget();

    mutable std::mutex m_mutex;
    Lru                m_lru;       // front = most recently used
    Index              m_index;
    size_t             m_maxBytes;
    size_t             m_bytesUsed{0};

    friend class FrameLease;
};

/**
 * RAII handle returned by FrameCache::get. Holds a strong reference to the
 * cached frame so eviction can run concurrently without freeing the data
 * the caller is mid-upload from. Deref returns the frame; bool-convertible
 * for the standard `if (auto lease = cache.get(...)) { ... }` pattern.
 */
class FrameLease {
public:
    FrameLease() = default;
    explicit FrameLease(std::shared_ptr<const DecodedFrame> frame)
        : m_frame(std::move(frame)) {}

    bool valid() const noexcept { return static_cast<bool>(m_frame); }
    explicit operator bool() const noexcept { return valid(); }

    const DecodedFrame& operator*()  const { return *m_frame; }
    const DecodedFrame* operator->() const { return m_frame.get(); }
    const DecodedFrame* get()        const { return m_frame.get(); }

private:
    std::shared_ptr<const DecodedFrame> m_frame;
};

} // namespace entity
