#pragma once

#include <functional>

namespace entity {

/**
 * SeekSyncController — holds the seek-sync gate until every active decoder
 * (video + audio) reports its target frame ready, then releases it.
 *
 * Owned by Engine (std::unique_ptr); ticked from Engine::update() right after
 * AudioSystem::update (editor thread only — it reads the editor-owned worker
 * maps). The gate itself lives on Timeline and is set by Timeline::play()
 * on the ->Playing transition.
 *
 * Testable seam: readiness checks are injected as std::functions, not
 * hardcoded system pointers, so unit tests can supply fakes without standing
 * up a full Engine.
 *
 * Placement rationale (ADR-0014): Director-side code (Timeline, AudioSystem,
 * PlaybackTimeAuthority) must not name Renderer-side code (DecodeSystem,
 * FrameCache). Engine already holds shortcuts to both sides, so the controller
 * lives here.
 */
class SeekSyncController {
public:
    // Readiness predicates — set once at construction from Engine::initialize.
    // Each returns true when all active clips that carry the respective kind
    // of decoder have their target frame ready.
    //
    // videoReady: no active video-clip worker is still mid-seek / pre-init.
    // audioReady: all active audio workers have kAudioPrerollFrames buffered.
    //
    // Either may be left nullptr / empty; an empty/null predicate counts as
    // "always ready" so a build with no AudioSystem wired still works.
    using ReadinessFn = std::function<bool()>;

    explicit SeekSyncController(ReadinessFn videoReady, ReadinessFn audioReady);

    // Called once per editor tick (Engine::update, after AudioSystem::update).
    // No-ops when the gate is not engaged.
    //
    // timeline: non-owning pointer — must outlive every tick() call.
    //
    // The Timeline pointer is passed per-tick (not stored at construction) to
    // keep the dependency graph flat and to avoid a stale pointer if Timeline
    // is ever recreated — Engine constructs a new Timeline on project load.
    void tick(class Timeline* timeline);

    // Timeout: if the gate is held longer than this, release it with a warning.
    // 3 000 ms — generous enough for slow GOP seeks on 4K material.
    static constexpr int kPrerollTimeoutMs = 3000;

private:
    ReadinessFn m_videoReady;
    ReadinessFn m_audioReady;

    // true after the gate first transitions clear→active on a tick(); reset to
    // false on any release (readiness satisfied, timeout, or gate externally
    // cleared).
    bool m_primed{false};

    // Last-seen gate state.  Used to detect the false→true transition so the
    // controller arms correctly even when the caller sleeps between Timeline::
    // play() and the first tick().  The engage timestamp itself is owned by
    // Timeline (m_seekSyncEngageTimeNs), stamped at play()-time.
    bool m_lastGateState{false};
};

} // namespace entity
