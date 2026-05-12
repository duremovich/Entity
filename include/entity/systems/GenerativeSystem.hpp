#pragma once

#include "entity/systems/System.hpp"

namespace entity {

class Timeline;
class InputBus;

/**
 * GenerativeSystem — ticks procedural-content layers (GenerativeLayer-marked
 * entities) once per editor frame.
 *
 * For each active layer (current timeline frame within [Layer::startFrame,
 * Layer::startFrame + Layer::duration)), advance the kind-specific
 * simulation state. Kinds are distinguished by component composition
 * (ADR-0016):
 *   - Muncher → requires MunchersGameState; system increments simFrame.
 *
 * Editor-thread only (ADR-0014). The show thread reads the baked simulation
 * via SceneSnapshot (Phase 2 wiring); no registry writes happen on the show
 * thread.
 *
 * Editor-stall behavior (V1): the simulation freezes during editor stalls
 * (modal dialogs, slow project load). Documented limitation. Phase 2's
 * snapshot-bake will let the show thread re-tick locally during stalls
 * without writing the registry — same shape as the AnimationSystem
 * NEW-07 fix.
 */
class GenerativeSystem : public System {
public:
    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "GenerativeSystem"; }

    // Timeline pointer is injected after construction, same pattern as
    // AnimationSystem. update() is a no-op until set.
    void setTimeline(Timeline* timeline) { m_timeline = timeline; }

    // Optional — wire the cross-system input bus so the Muncher kind can
    // read its player axes ("muncher.input.x" / ".y") each tick. Without
    // it, the Muncher just sits still.
    void setInputBus(InputBus* inputBus) { m_inputBus = inputBus; }

private:
    Timeline* m_timeline{nullptr};
    InputBus* m_inputBus{nullptr};
};

} // namespace entity
