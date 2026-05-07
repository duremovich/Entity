#pragma once

#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include <entt/entt.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace entity {

class Timeline;
class ProjectManager;
struct Clip;

// One per-tick active-clip tuple. Subset of the RenderFrame bus message
// that subtask 8 turns into the wire format -- transform / opacity /
// blendMode / targetScreen are added there. For now this is a plain
// in-process value type consumed by PlaybackPresenter on the Renderer
// side (see include/entity/renderer/PlaybackPresenter.hpp).
struct ActiveClip {
    entt::entity entity{entt::null};
    uint32_t     descriptorSlot{UINT32_MAX};
    FrameNumber  mediaFrame{0};
    std::string  ocioOverride;  // empty = no per-clip MediaBin override
};

// PlaybackTimeAuthority -- Director-side half of the old
// PlaybackController. Owns frame timing (deltaTime / elapsedTime /
// frameCount), the clip-frame math (mapToMediaFrame /
// isClipActiveAtFrame), and the per-tick active-set computation.
//
// Knows nothing about IRenderer / FrameCache / DecodeSystem / GPU upload
// -- the Renderer-side consumer is PlaybackPresenter. Per the Phase D
// entry boundary rules (include/entity/director/CLAUDE.md): writes to
// the registry happen on the Director's tick; the active set is built
// from registry reads of Clip + VideoTexture and an override lookup
// against ProjectManager.
//
// Construction order: Director owns this; needs Timeline at construction
// (Director's m_timeline is built first) and ProjectManager via
// `setProjectManager` once that's also up.
class PlaybackTimeAuthority {
public:
    PlaybackTimeAuthority(entt::registry& registry, Timeline* timeline);
    ~PlaybackTimeAuthority();

    PlaybackTimeAuthority(const PlaybackTimeAuthority&) = delete;
    PlaybackTimeAuthority& operator=(const PlaybackTimeAuthority&) = delete;

    // Per-clip MediaBin OCIO input color-space override comes from
    // ProjectManager::findEntry. Optional -- pre-init buildActiveSet
    // returns empty `ocioOverride` strings.
    void setProjectManager(ProjectManager* p) { m_projectManager = p; }

    // Per-tick hooks driven by Engine's main loop.
    void startTiming();
    void updateTiming();
    void incrementFrameCount() { ++m_frameCount; }

    double   getDeltaTime()   const { return m_deltaTime; }
    double   getElapsedTime() const { return m_elapsedTime; }
    uint64_t getFrameCount()  const { return m_frameCount; }

    // Clip-frame math -- pure functions, no side effects, callable from
    // anywhere (PlaybackPresenter's getCurrentVideoFrame uses these too).
    bool        isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const;
    FrameNumber mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const;

    // Entity-aware overload (Phase C — Normal continuation phase). When the
    // entity carries a ClipPlaybackPhase component with `inContinuation` set,
    // the media frame is derived from `sourcePhaseFrames` instead of the
    // (frozen) timeline-frame delta -- letting Loop/PingPong clips keep
    // cycling while the playhead is parked at a section break. Falls through
    // to the two-arg overload when no ClipPlaybackPhase exists or the clip
    // is not in continuation, so existing call sites keep their semantics.
    FrameNumber mapToMediaFrame(entt::entity entity,
                                const Clip& clip,
                                FrameNumber timelineFrame) const;

    // Walks `view<Clip, VideoTexture>` at the timeline's current frame,
    // emits one ActiveClip tuple per allocated + active clip. Reuses
    // `out` (clears + appends) so a long-lived caller can avoid reallocs.
    // No-op when timeline is unbound.
    void buildActiveSet(std::vector<ActiveClip>& out) const;

    // Per-tick Director->Renderer state snapshot. Fills the bus message
    // body in place (clears its activeClips/wantedFrames vectors first
    // so a long-lived caller can avoid reallocs). Stamps frameNumber,
    // deltaTime, playState, then walks the registry to produce one
    // ClipRenderState per allocated + active clip with `slot`,
    // `mediaFrame`, `ocioOverride`, plus the optional render fields
    // (transform / opacity / blendMode / targetScreen) when their
    // components exist. wantedFrames is left empty for now -- DecodeSystem
    // still drives itself off the registry until a later subtask
    // collapses that path through the bus too. No-op when timeline is
    // unbound (RenderFrame is reset to defaults).
    void buildRenderFrame(bus::RenderFrame& out) const;

    // Read-only Timeline accessor for callers that want to consult time
    // state alongside the math (PlaybackPresenter::getCurrentVideoFrame).
    const Timeline* timeline() const { return m_timeline; }

    // Phase D — section fade envelopes. Returns the auto-fade opacity
    // multiplier (∈ [0, 1]) for `clip` at the timeline's current frame:
    //   - 1.0 when the clip's start/end aren't aligned with any section
    //     break that has fadeSeconds > 0.
    //   - Ramps 0→1 across `fadeSeconds` from clip start when the start
    //     coincides (±1 timeline-frame snap) with such a break.
    //   - Ramps 1→0 across `fadeSeconds` ending at clip end when the end
    //     coincides with such a break.
    //   - Both ends aligned: the min of the two ramps applies (a clip
    //     shorter than fadeIn + fadeOut would otherwise overshoot).
    // Pure read of Timeline + Clip; no side effects. Public so the
    // AssertClipFadeMultiplierCommand can poll it from script tests.
    float computeSectionFadeMultiplier(const Clip& clip) const;

private:
    std::string lookupInputColorSpaceOverride(const Clip& clip) const;

    entt::registry& m_registry;
    Timeline*       m_timeline{nullptr};
    ProjectManager* m_projectManager{nullptr};

    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    TimePoint m_startTime;
    TimePoint m_lastFrameTime;
    double    m_deltaTime{0.0};
    double    m_elapsedTime{0.0};
    uint64_t  m_frameCount{0};
};

} // namespace entity
