#pragma once

#include "entity/core/Types.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace entity {

class Timeline;

/**
 * PrecompDefinition — one authored precomp (ADR-0029 Decision 2).
 *
 * `id` is an opaque stable string ("precomp-" + 16 random hex chars);
 * instances reference definitions by it, following the
 * persist-by-stable-name idiom. `name` is display-only. `timeline` is a
 * real Timeline sharing the engine registry so the authoring tab gets
 * tracks, clip-edit machinery, and an edit playhead for free — it is
 * NEVER handed to playback systems (Director's Timeline remains the only
 * transport Engine/PTA/SectionScheduler/DecodeSystem/AudioSystem read).
 * durationFrames is authoritative; the Timeline's Timecode duration is
 * derived from it via frameToTime.
 */
struct PrecompDefinition {
    std::string   id;
    std::string   name;
    std::uint32_t canvasWidth{1920};
    std::uint32_t canvasHeight{1080};
    double        frameRate{30.0};
    FrameNumber   durationFrames{0};
    std::uint64_t version{1};
    std::unique_ptr<Timeline> timeline;
};

/**
 * PrecompLibrary — Engine-owned registry of precomp definitions
 * (ADR-0029 Decision 2). Editor-thread only, like all authoring state.
 * touch() bumps a definition's version; PrecompInstancingSystem (Phase C)
 * resyncs instances whose materializedVersion lags.
 */
class PrecompLibrary {
public:
    explicit PrecompLibrary(entt::registry& registry);
    ~PrecompLibrary();

    PrecompLibrary(const PrecompLibrary&) = delete;
    PrecompLibrary& operator=(const PrecompLibrary&) = delete;

    /**
     * Create a definition with a fresh unique id and a Timeline configured
     * to (frameRate, durationFrames). Phase A creates no tracks — zero
     * registry side effects; track authoring arrives with persistence
     * (Phase F) and the editor tab (Phase G). Returns a non-owning pointer
     * (stable — definitions are held by unique_ptr).
     */
    PrecompDefinition* createDefinition(const std::string& name,
                                        std::uint32_t canvasWidth,
                                        std::uint32_t canvasHeight,
                                        double frameRate,
                                        FrameNumber durationFrames);

    PrecompDefinition*       findDefinition(const std::string& id);
    const PrecompDefinition* findDefinition(const std::string& id) const;

    // Version bump — the resync trigger (ADR-0029 Decision 9). Returns
    // false if the id is unknown.
    bool touch(const std::string& id);

    const std::vector<std::unique_ptr<PrecompDefinition>>& definitions() const {
        return m_definitions;
    }
    std::size_t count() const { return m_definitions.size(); }

    // v1 rule: a definition cannot be deleted while instances reference
    // it. Phase A stub — true iff the definition exists; Phase C replaces
    // the body with an instance-refcount check.
    bool canDelete(const std::string& id) const;

private:
    std::string generateId() const;

    entt::registry& m_registry;
    std::vector<std::unique_ptr<PrecompDefinition>> m_definitions;
};

} // namespace entity
