#pragma once

#include "entity/systems/System.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/core/Types.hpp"

namespace entity {

/**
 * BufferSystem - Manages frame buffer state and queries.
 *
 * Extracted from FrameBuffer component to follow ECS/DOD principles.
 * Components are pure data, Systems contain logic.
 *
 * This system provides buffer status queries and management functions
 * that operate on FrameBuffer component data. Used by DecodeSystem,
 * RenderSystem, and other systems for buffer state inspection.
 *
 * Design Notes:
 * - This is a "utility system" - update() is no-op, logic is called on-demand
 * - Methods are const to emphasize read-only queries
 * - Thread-safe due to atomic operations on FrameBuffer component
 */
class BufferSystem : public System {
public:
    /**
     * No per-frame update needed - this system provides utility functions
     * called on-demand by other systems (DecodeSystem, RenderSystem, etc.)
     */
    void update(entt::registry& registry, float deltaTime) override {
        // BufferSystem is a utility system with no per-frame work
        // Logic is invoked on-demand by other systems
    }

    /**
     * Get system name for debugging and logging.
     */
    const char* getName() const override {
        return "BufferSystem";
    }

    /**
     * Check if buffer has frames available for reading.
     *
     * Extracted from FrameBuffer::hasFrames() to maintain pure data component.
     *
     * @param buffer The FrameBuffer component to check
     * @return true if buffer contains at least one frame, false otherwise
     */
    bool hasFrames(const FrameBuffer& buffer) const {
        return buffer.bufferedFrames.load() > 0;
    }

    /**
     * Check if buffer is sufficiently filled for stable playback.
     *
     * Extracted from FrameBuffer::isReady() to maintain pure data component.
     *
     * A buffer is considered "ready" when it has at least 8 frames buffered,
     * which provides ~266ms of content at 30fps, enough to handle
     * frame decode jitter and minor stalls.
     *
     * @param buffer The FrameBuffer component to check
     * @return true if buffer has at least 8 frames, false otherwise
     */
    bool isReady(const FrameBuffer& buffer) const {
        constexpr uint32_t MIN_READY_FRAMES = 8;
        return buffer.bufferedFrames.load() >= MIN_READY_FRAMES;
    }

    /**
     * Get buffer fill percentage (0.0 to 1.0).
     *
     * Extracted from FrameBuffer::getFillPercentage() to maintain pure data component.
     *
     * Useful for UI visualization (progress bars, debug displays) and
     * adaptive playback strategies (e.g., pause if buffer drops below threshold).
     *
     * @param buffer The FrameBuffer component to check
     * @return Buffer fill ratio from 0.0 (empty) to 1.0 (full)
     */
    float getFillPercentage(const FrameBuffer& buffer) const {
        return static_cast<float>(buffer.bufferedFrames.load()) /
               static_cast<float>(DEFAULT_FRAME_BUFFER_SIZE);
    }

    /**
     * Check if buffer is currently in buffering state.
     *
     * @param buffer The FrameBuffer component to check
     * @return true if buffer is still filling, false if ready for playback
     */
    bool isBuffering(const FrameBuffer& buffer) const {
        return buffer.isBuffering.load();
    }

    /**
     * Get current number of buffered frames.
     *
     * @param buffer The FrameBuffer component to check
     * @return Number of frames currently in the buffer
     */
    uint32_t getBufferedFrameCount(const FrameBuffer& buffer) const {
        return buffer.bufferedFrames.load();
    }
};

} // namespace entity
