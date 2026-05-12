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
#include "entity/input/InputBus.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace entity {

namespace {

// Snap a continuous 2D input (analog stick / WASD / OSC fader pair) to a
// cardinal direction. Greatest-magnitude axis wins, sub-deadzone reads as
// zero. Returns (-1, 0, or +1) for each axis; only one is non-zero at a
// time. Matches classic Pac-Man "single cardinal direction at a time"
// rule; this becomes "queue next direction, change at intersection" once
// maze walls land.
void snapInputToCardinal(float inputX, float inputY, float deadzone,
                          float& outVX, float& outVY) {
    const float ax = std::fabs(inputX);
    const float ay = std::fabs(inputY);
    outVX = 0.0f;
    outVY = 0.0f;
    if (ax < deadzone && ay < deadzone) return;
    if (ax >= ay) {
        outVX = (inputX > 0.0f) ? 1.0f : -1.0f;
    } else {
        outVY = (inputY > 0.0f) ? 1.0f : -1.0f;
    }
}

void tickMuncher(MunchersGameState& s, float inputX, float inputY) {
    ++s.simFrame;

    // Persist last-read axes in the state so the snapshot can carry them
    // through to the show side (debug overlays etc.).
    s.inputX = std::clamp(inputX, -1.0f, 1.0f);
    s.inputY = std::clamp(inputY, -1.0f, 1.0f);

    float dirX = 0.0f, dirY = 0.0f;
    snapInputToCardinal(s.inputX, s.inputY, s.inputDeadzone, dirX, dirY);

    s.muncherVelX = dirX * s.speedPerTick;
    s.muncherVelY = dirY * s.speedPerTick;

    s.muncherX = std::clamp(s.muncherX + s.muncherVelX, 0.0f, 1.0f);
    s.muncherY = std::clamp(s.muncherY + s.muncherVelY, 0.0f, 1.0f);
}

} // namespace

void GenerativeSystem::initialize(entt::registry& /*registry*/) {
    std::cout << "GenerativeSystem initialized" << std::endl;
}

void GenerativeSystem::update(entt::registry& registry, float /*deltaTime*/) {
    ZoneScopedN("GenerativeSystem::update");
    if (!m_timeline) return;

    const FrameNumber currentFrame = m_timeline->getCurrentFrame();

    // Read once per tick — input channels are shared across all Muncher
    // layers. (v3+: per-layer input routing if a project ever wants two
    // independent Munchers on different controllers.)
    const float inputX = m_inputBus ? m_inputBus->getFloat("muncher.input.x", 0.0f) : 0.0f;
    const float inputY = m_inputBus ? m_inputBus->getFloat("muncher.input.y", 0.0f) : 0.0f;

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
        tickMuncher(state, inputX, inputY);
    }

    // Future generative kinds dispatch here: view<Layer, GenerativeLayer,
    // ParticlesState>(), view<Layer, GenerativeLayer, WaveState>(), etc.
}

void GenerativeSystem::shutdown(entt::registry& /*registry*/) {
    std::cout << "GenerativeSystem shutdown" << std::endl;
}

} // namespace entity
