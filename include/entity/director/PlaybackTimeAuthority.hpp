#pragma once

#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include "entity/director/RateSource.hpp"
#include "entity/director/SystemRateSource.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include <entt/entt.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace entity {

class Timeline;
class ProjectManager;
struct Clip;

namespace remote { class RemoteControlStore; }

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

    // Per-layer effects registry (issue #54). Provides ParamSchema lookups
    // used during snapshot bake to marshal EffectParameters → paramBlob.
    // Set once at Engine startup; the registry is read-only after
    // registerBuiltins.
    void setEffectKindRegistry(const effects::EffectKindRegistry* r) {
        m_effectKindRegistry = r;
    }

    // ADR-0028: live remote-control store. The show thread reads it lock-free
    // (indexed atomic loads); the editor wires this once at startup and after
    // project load. nullptr = no store (remote overlay disabled).
    void setRemoteControlStore(const remote::RemoteControlStore* store) {
        m_remoteStore = store;
    }

    // Phase B wires the audio device's rate source in here. nullptr =>
    // system clock only (today's behaviour).
    void setAudioRateSource(RateSource* audioRate) { m_audioRate = audioRate; }

    // The currently-selected master rate source's now(), in seconds.
    // Thread-safe read (used by SectionScheduler in Phase G). Never
    // mutates selector bookkeeping.
    double rateNow() const {
        RateSource* s = m_activeRate.load(std::memory_order_acquire);
        return s ? s->now() : 0.0;
    }

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

    // 2026-05-23 — Normal-mode break-aligned active-window extension.
    // When a Normal-mode clip's timeline-end aligns with a section break
    // AND the clip has more source content to play past that point, the
    // active window extends past clip.duration up to the source out point.
    // Returns clip.duration when no extension applies (Locked clip,
    // non-break-aligned end, or sourceTimelineFrames <= duration). Public
    // so SectionScheduler can seed continuation / post-break anchors for
    // break-aligned-end Normal clips that the original spanning-check
    // would have skipped. See ADR-0012 amendment 2026-05-23.
    FrameNumber computeExtendedDuration(const Clip& clip) const;

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

    // Stage 3 split of buildRenderFrame.
    //
    // Editor half (called once per editor frame from Engine::run()):
    // Walks Screen / OutputSurface / Projector / OutputDisplay registry
    // views and fills `out`. Published latest-wins on D2R as a
    // bus::SceneSnapshot so the show thread never reads the registry.
    void buildSceneSnapshot(bus::SceneSnapshot& out) const;

    // Show half (called per show tick from Engine::showThreadMain()):
    // Merges `scene` into `out` alongside the per-tick clip state
    // (activeClips, playState, frameNumber, deltaTime). Zero registry reads —
    // all clip/layer/phase data comes from scene.clipCatalog, populated by
    // buildSceneSnapshot on the editor thread.
    void buildRenderFrame(bus::RenderFrame& out,
                          const bus::SceneSnapshot& scene) const;

    // Legacy single-call variant kept for call sites that still run on
    // the main thread (launcher mode, headless script path). Snapshots
    // scene state inline rather than merging a cached SceneSnapshot.
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
    //
    // Generative-layer overload — same math, operating on (start, end)
    // directly instead of Clip fields. Lets generative layers (Text,
    // Muncher) honor section fades without inventing a fake Clip. The
    // Clip version is a trampoline onto this overload.
    float computeSectionFadeMultiplier(FrameNumber layerStart,
                                       FrameNumber layerEnd) const;
    float computeSectionFadeMultiplier(const Clip& clip) const;

private:
    RateSource* selectRateSource();

    std::string lookupInputColorSpaceOverride(const Clip& clip) const;

    // Phase 6 — hold + fade-after-break. Returns the number of timeline
    // frames the clip's tail extends past `endFrame` when a section break
    // sits at `endFrame` (±1 frame snap) with non-zero fadeSeconds; 0
    // otherwise. Used by isClipActiveAtFrame to keep the clip in the
    // active set during the post-end hold + fade window, and by
    // mapToMediaFrame to short-circuit to the last decoded source frame
    // inside that window.
    FrameNumber sectionFadeTailFrames(FrameNumber endFrame) const;

    entt::registry& m_registry;
    Timeline*       m_timeline{nullptr};
    ProjectManager* m_projectManager{nullptr};
    const effects::EffectKindRegistry*    m_effectKindRegistry{nullptr};
    const remote::RemoteControlStore*     m_remoteStore{nullptr};

    SystemRateSource           m_systemRate;
    RateSource*                m_audioRate{nullptr};
    std::atomic<RateSource*>   m_activeRate{nullptr};
    RateSource*                m_lastSelected{nullptr};
    double                     m_lastNow{0.0};
    double                     m_startNow{0.0};
    double                     m_deltaTime{0.0};
    double                     m_elapsedTime{0.0};
    uint64_t                   m_frameCount{0};
};

} // namespace entity
