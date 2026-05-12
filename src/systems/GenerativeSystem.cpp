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

// Pellet bitset helpers. 16×16 grid packed into 4 uint64 — cell (cx, cy)
// is bit `cy * 16 + cx` of the 256-bit set, stored low-bit-first.
constexpr int pelletBitIndex(int cx, int cy) {
    return cy * MunchersGameState::kPelletGridSide + cx;
}

bool pelletGet(const std::array<std::uint64_t, 4>& bits, int cx, int cy) {
    const int idx = pelletBitIndex(cx, cy);
    return (bits[idx / 64] >> (idx % 64)) & 1ull;
}

void pelletClear(std::array<std::uint64_t, 4>& bits, int cx, int cy) {
    const int idx = pelletBitIndex(cx, cy);
    bits[idx / 64] &= ~(1ull << (idx % 64));
}

bool pelletGridEmpty(const std::array<std::uint64_t, 4>& bits) {
    return (bits[0] | bits[1] | bits[2] | bits[3]) == 0ull;
}

void pelletGridFillAll(std::array<std::uint64_t, 4>& bits) {
    bits[0] = bits[1] = bits[2] = bits[3] = ~0ull;
}

// Cell index for a normalized position. Clamps to grid bounds.
int positionToCell(float p) {
    int c = static_cast<int>(p * MunchersGameState::kPelletGridSide);
    if (c < 0) c = 0;
    if (c >= MunchersGameState::kPelletGridSide)
        c = MunchersGameState::kPelletGridSide - 1;
    return c;
}

void initMuncherStateIfNeeded(MunchersGameState& s) {
    if (s.initialized) return;
    pelletGridFillAll(s.pelletBits);
    // Spawn ghosts away from the player's starting cell. Top-left,
    // top-right, bottom-right corners; bottom-left stays free for the
    // player to safely start at center.
    s.ghosts[0] = MunchersGameState::Ghost{0.05f, 0.05f, 0.0f, 0.0f};
    s.ghosts[1] = MunchersGameState::Ghost{0.95f, 0.05f, 0.0f, 0.0f};
    s.ghosts[2] = MunchersGameState::Ghost{0.95f, 0.95f, 0.0f, 0.0f};
    s.initialized = true;
}

void tickGhost(MunchersGameState::Ghost& g, float playerX, float playerY,
                std::uint64_t simFrame, int ghostIndex, float speedPerTick) {
    // Re-pick a cardinal heading every kGhostDecisionTicks, staggered per
    // ghost so they don't change direction in lockstep. Heading is the
    // cardinal direction closest to the player vector; magnitude ties
    // break by axis order (X wins), same as the player's snap rule.
    const std::uint64_t cadence = MunchersGameState::kGhostDecisionTicks;
    if ((simFrame + static_cast<std::uint64_t>(ghostIndex) * 13ull) % cadence == 0) {
        const float dx = playerX - g.x;
        const float dy = playerY - g.y;
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        if (ax >= ay) {
            g.vx = (dx > 0.0f) ? speedPerTick : -speedPerTick;
            g.vy = 0.0f;
        } else {
            g.vx = 0.0f;
            g.vy = (dy > 0.0f) ? speedPerTick : -speedPerTick;
        }
    }
    g.x = std::clamp(g.x + g.vx, 0.0f, 1.0f);
    g.y = std::clamp(g.y + g.vy, 0.0f, 1.0f);
    // Force a re-pick next tick if we hit an edge.
    if (g.x <= 0.0f || g.x >= 1.0f) g.vx = 0.0f;
    if (g.y <= 0.0f || g.y >= 1.0f) g.vy = 0.0f;
}

void tickMuncher(MunchersGameState& s, float inputX, float inputY) {
    ++s.simFrame;
    initMuncherStateIfNeeded(s);

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

    // Eat any pellet under the player's current cell. Tiny score++ per
    // pellet for now; classic Pac-Man scores 10 per dot, but the
    // absolute number is meaningless without a score display.
    const int playerCellX = positionToCell(s.muncherX);
    const int playerCellY = positionToCell(s.muncherY);
    if (pelletGet(s.pelletBits, playerCellX, playerCellY)) {
        pelletClear(s.pelletBits, playerCellX, playerCellY);
        if (s.score < UINT16_MAX) ++s.score;
    }
    // Respawn the grid when emptied — keeps the demo perpetually fun.
    // Later this advances a "level" counter and bumps ghost speed.
    if (pelletGridEmpty(s.pelletBits)) {
        pelletGridFillAll(s.pelletBits);
    }

    // Move ghosts and check collision. Collision = closer than half a
    // pellet cell. On hit, recenter the player and start a short
    // invincibility window so a clump of ghosts can't repeat-kill.
    constexpr float kCollisionDist = 1.0f / MunchersGameState::kPelletGridSide * 0.6f;
    for (int gi = 0; gi < MunchersGameState::kNumGhosts; ++gi) {
        auto& g = s.ghosts[gi];
        tickGhost(g, s.muncherX, s.muncherY, s.simFrame, gi, s.ghostSpeedPerTick);
        const float dx = g.x - s.muncherX;
        const float dy = g.y - s.muncherY;
        const float dist2 = dx * dx + dy * dy;
        const bool stillInvincible =
            s.lastHitFrame != 0 &&
            (s.simFrame - s.lastHitFrame) < MunchersGameState::kInvincibilityTicks;
        if (!stillInvincible && dist2 < kCollisionDist * kCollisionDist) {
            s.muncherX = 0.5f;
            s.muncherY = 0.5f;
            s.muncherVelX = 0.0f;
            s.muncherVelY = 0.0f;
            if (s.lives > 0) --s.lives;
            // Full reset when no lives left: refill pellets, reset score,
            // grant 3 lives, send ghosts back to corners.
            if (s.lives == 0) {
                pelletGridFillAll(s.pelletBits);
                s.score = 0;
                s.lives = 3;
                s.ghosts[0] = MunchersGameState::Ghost{0.05f, 0.05f, 0.0f, 0.0f};
                s.ghosts[1] = MunchersGameState::Ghost{0.95f, 0.05f, 0.0f, 0.0f};
                s.ghosts[2] = MunchersGameState::Ghost{0.95f, 0.95f, 0.0f, 0.0f};
            }
            s.lastHitFrame = s.simFrame;
            break;  // one collision per tick
        }
    }
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
