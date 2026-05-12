/**
 * GenerativeSystem Implementation
 *
 * Editor-thread tick for procedural-content layers. V1 skeleton: advances a
 * per-layer simFrame counter while the layer is active in the timeline.
 * Real per-kind simulation logic (Muncher gameplay, particle physics, ...)
 * is dispatched here once the show-side rendering and input plumbing land.
 */

#include "entity/systems/GenerativeSystem.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/MunchersGameState.hpp"

#include <iostream>

namespace entity {

void GenerativeSystem::initialize(entt::registry& /*registry*/) {
    std::cout << "GenerativeSystem initialized" << std::endl;
}

void GenerativeSystem::update(entt::registry& registry, float /*deltaTime*/) {
    ZoneScopedN("GenerativeSystem::update");
    if (!m_timeline) return;

    const FrameNumber currentFrame = m_timeline->getCurrentFrame();

    // Muncher kind — discriminator is MunchersGameState component presence
    // (no Kind enum dispatch; ADR-0016 composition rule).
    auto munchers = registry.view<Layer, GenerativeLayer, MunchersGameState>();
    for (auto entity : munchers) {
        const auto& layer = munchers.get<Layer>(entity);
        if (currentFrame < layer.startFrame ||
            currentFrame >= layer.startFrame + layer.duration) {
            continue;  // inactive at this frame
        }
        auto& state = munchers.get<MunchersGameState>(entity);
        ++state.simFrame;
    }

    // Future generative kinds dispatch here: view<Layer, GenerativeLayer,
    // ParticlesState>(), view<Layer, GenerativeLayer, WaveState>(), etc.
}

void GenerativeSystem::shutdown(entt::registry& /*registry*/) {
    std::cout << "GenerativeSystem shutdown" << std::endl;
}

} // namespace entity
