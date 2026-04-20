#pragma once

#include "entity/core/Types.hpp"
#include <entt/entt.hpp>
#include <chrono>

namespace entity {

class IRenderer;
class Timeline;
class DecodeSystem;
struct Clip;
struct DecodedFrame;

/**
 * PlaybackController — owns frame timing and per-frame seek-aware playback
 * coordination.
 *
 * Responsibilities:
 *   1. Frame timing: deltaTime, elapsedTime, frameCount. `updateTiming()` is
 *      called once per main-loop iteration.
 *   2. Clip-frame math: `isClipActiveAtFrame` + `mapToMediaFrame` translate a
 *      timeline frame into the source media frame for a clip, honoring
 *      playback mode (Freeze/Loop/PingPong) and timeline↔source fps ratio.
 *   3. Per-frame coordination: `updateClipVideos()` walks all Clip+VideoTexture
 *      entities, pulls frames from ring buffers (or falls back to sync decode
 *      when paused/scrubbing), and uploads them to GPU slots. Respects the
 *      decode worker's `seekPending` flag to avoid showing stale frames.
 *   4. `getCurrentVideoFrame()` returns the frame that should be displayed
 *      *right now* for the selected/active clip (with stale-frame rejection).
 *
 * Dependencies are injected at construction (registry, Timeline, renderer);
 * DecodeSystem is injected later via `setDecodeSystem()` because it's
 * registered after this controller is created.
 *
 * Transport (play/pause/stop/seek) lives in Timeline — this class consumes
 * that state but does not own it.
 */
class PlaybackController {
public:
    PlaybackController(entt::registry& registry, Timeline* timeline, IRenderer* renderer);
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    void setDecodeSystem(DecodeSystem* s) { m_decodeSystem = s; }

    // Per-frame hooks from the engine main loop.
    void startTiming();
    void updateTiming();
    void incrementFrameCount() { ++m_frameCount; }
    void updateClipVideos();

    // Queries
    double getDeltaTime() const { return m_deltaTime; }
    double getElapsedTime() const { return m_elapsedTime; }
    uint64_t getFrameCount() const { return m_frameCount; }
    const DecodedFrame* getCurrentVideoFrame() const;

    // Clip-frame math
    bool isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const;
    FrameNumber mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const;

private:
    entt::registry& m_registry;
    Timeline* m_timeline{nullptr};
    IRenderer* m_renderer{nullptr};
    DecodeSystem* m_decodeSystem{nullptr};

    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    TimePoint m_startTime;
    TimePoint m_lastFrameTime;
    double m_deltaTime{0.0};
    double m_elapsedTime{0.0};
    uint64_t m_frameCount{0};
};

} // namespace entity
