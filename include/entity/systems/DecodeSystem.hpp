#pragma once

#include "entity/systems/System.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/FrameRingBuffer.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Clip.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <limits>

namespace entity {

// Forward declarations
class Timeline;

/**
 * DecodeWorker - Per-clip decode thread state.
 *
 * Manages a single decode thread that fills a FrameRingBuffer.
 * Each clip entity with a FrameBuffer component gets its own worker.
 */
struct DecodeWorker {
    std::unique_ptr<Decoder> decoder;           // Decoder instance (owned by worker)
    std::shared_ptr<FrameRingBuffer> ringBuffer; // Shared with FrameBuffer component
    std::thread thread;                          // Decode thread

    // Thread control
    std::atomic<bool> running{false};            // True while thread should run
    std::atomic<bool> paused{false};             // True to pause decoding
    std::atomic<bool> seekPending{false};        // True when seek is requested
    std::atomic<FrameNumber> seekTarget{0};      // Frame to seek to
    std::atomic<FrameNumber> currentFrame{0};    // Last decoded frame
    std::atomic<FrameNumber> targetFrame{0};     // Target frame to decode ahead to
    // Sentinel value for "no frame requested yet" (used for discontinuity detection)
    static constexpr FrameNumber INVALID_FRAME = std::numeric_limits<FrameNumber>::min();
    std::atomic<FrameNumber> lastRequestedFrame{INVALID_FRAME}; // Last frame requested by render (for discontinuity detection)

    // Deferred initialization (decoder opened in worker thread, not main thread)
    std::atomic<bool> initialized{false};        // True once decoder is opened
    std::atomic<bool> initFailed{false};         // True if initialization failed
    std::string filepath;                        // File path for deferred open
    MediaType mediaType{MediaType::Unknown};     // Media type for decoder creation

    // Playback mode for loop/ping-pong support
    PlaybackMode playbackMode{PlaybackMode::Freeze};
    FrameNumber totalMediaFrames{0};             // Total frames in source media
    std::atomic<bool> pingPongReverse{false};    // True when ping-pong is playing backward

    // Synchronization for pause/resume
    std::mutex mutex;
    std::condition_variable cv;
};

/**
 * DecodeSystem - ECS system for managing background decode threads.
 *
 * ALWAYS-WARM BUFFERING FOR ZERO-LATENCY PLAYBACK:
 * Decode workers run continuously, targeting the current timeline position
 * regardless of playback state (playing, paused, or stopped). This ensures
 * buffers are always warm at the playhead position - when Play is pressed,
 * frames are already decoded and playback starts instantly.
 *
 * Responsibilities:
 * - Spawn/manage decode threads for each clip with a FrameBuffer
 * - Fill FrameRingBuffers with decoded frames ahead of current position
 * - Handle seek/scrub events (clear buffer, seek decoder, refill)
 * - Keep buffers warm even when timeline is paused (critical for live use)
 *
 * Threading Model:
 * - Main thread: update() called each frame, manages worker state
 * - Decode threads: One per clip, fills ring buffer independently
 * - Communication via atomics and condition variables
 *
 * Design Notes:
 * - Decoders are NOT thread-safe, each worker owns its decoder
 * - Ring buffers are lock-free for producer/consumer access
 * - Seek events clear buffer and signal decode thread to reposition
 * - Workers NEVER pause based on timeline state (always-warm buffering)
 */
class DecodeSystem : public System {
public:
    DecodeSystem();
    ~DecodeSystem();

    /**
     * Set the timeline for frame position tracking.
     */
    void setTimeline(Timeline* timeline) { m_timeline = timeline; }

    /**
     * Initialize the system.
     */
    void initialize(entt::registry& registry) override;

    /**
     * Per-frame update - manages worker threads and buffer state.
     *
     * Tasks:
     * - Start workers for new clips with FrameBuffer components
     * - Update target frames based on timeline position
     * - Handle seek events
     * - Clean up workers for removed clips
     */
    void update(entt::registry& registry, float deltaTime) override;

    /**
     * Shutdown all decode threads.
     */
    void shutdown(entt::registry& registry) override;

    /**
     * Get system name.
     */
    const char* getName() const override { return "DecodeSystem"; }

    /**
     * Request a seek for a specific clip.
     * Clears the buffer and repositions the decoder.
     */
    void seekClip(entt::entity clipEntity, FrameNumber frame);

    /**
     * Pause decoding for all clips.
     */
    void pauseAll();

    /**
     * Resume decoding for all clips.
     */
    void resumeAll();

    /**
     * Get decode worker for a clip (for status inspection).
     */
    const DecodeWorker* getWorker(entt::entity clipEntity) const;

private:
    /**
     * Create and start a decode worker for a clip entity.
     * @param initialFrame The media frame to start buffering from (usually current playhead position)
     */
    void createWorker(entt::entity entity, entt::registry& registry, FrameNumber initialFrame = 0);

    /**
     * Stop and remove a decode worker.
     */
    void destroyWorker(entt::entity entity);

    /**
     * Decode thread main loop.
     * Takes shared_ptr to ensure worker lifetime extends beyond thread execution.
     */
    static void decodeThreadFunc(std::shared_ptr<DecodeWorker> worker, entt::entity entity);

private:
    Timeline* m_timeline{nullptr};

    // Map of clip entity to decode worker (shared_ptr ensures thread-safe lifetime)
    std::unordered_map<entt::entity, std::shared_ptr<DecodeWorker>> m_workers;

    // Global pause state
    std::atomic<bool> m_globalPaused{false};

    // Scrubbing state tracking (to detect when scrubbing ends)
    bool m_wasScrubbing{false};

    // Decode-ahead configuration
    // Reduced from 16 to 8 for faster buffer refill on seek (always-warm buffering)
    static constexpr uint32_t DECODE_AHEAD_FRAMES = 8;   // Frames to buffer ahead
    static constexpr uint32_t MIN_BUFFER_FRAMES = 2;     // Minimum before resuming decode
};

} // namespace entity
