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

// Bitset helpers. 16×16 grid packed into 4 uint64 — cell (cx, cy) is
// bit `cy * 16 + cx` of the 256-bit set, stored low-bit-first. Shared
// by the wall and pellet grids.
constexpr int cellBitIndex(int cx, int cy) {
    return cy * MunchersGameState::kPelletGridSide + cx;
}

bool cellGet(const std::array<std::uint64_t, 4>& bits, int cx, int cy) {
    const int idx = cellBitIndex(cx, cy);
    return (bits[idx / 64] >> (idx % 64)) & 1ull;
}

void cellSet(std::array<std::uint64_t, 4>& bits, int cx, int cy) {
    const int idx = cellBitIndex(cx, cy);
    bits[idx / 64] |= (1ull << (idx % 64));
}

void cellClear(std::array<std::uint64_t, 4>& bits, int cx, int cy) {
    const int idx = cellBitIndex(cx, cy);
    bits[idx / 64] &= ~(1ull << (idx % 64));
}

bool bitsEmpty(const std::array<std::uint64_t, 4>& bits) {
    return (bits[0] | bits[1] | bits[2] | bits[3]) == 0ull;
}

// Cell index for a normalized position. Clamps to grid bounds.
int positionToCell(float p) {
    int c = static_cast<int>(p * MunchersGameState::kPelletGridSide);
    if (c < 0) c = 0;
    if (c >= MunchersGameState::kPelletGridSide)
        c = MunchersGameState::kPelletGridSide - 1;
    return c;
}

// Classic-ish 16×16 maze. `#` is wall, `.` is walkable (and gets a
// pellet at init). Symmetric L-R, opening at the top corners + bottom
// row so the player has a clean home column. Center has a horizontal
// corridor (the "tunnel" row in real Pac-Man).
constexpr const char* kMaze[MunchersGameState::kPelletGridSide] = {
    "################",   // 0
    "#......##......#",   // 1
    "#.####.##.####.#",   // 2
    "#..............#",   // 3
    "#.####.##.####.#",   // 4
    "#......##......#",   // 5
    "###.##.##.##.###",   // 6
    "#..............#",   // 7
    "#..............#",   // 8
    "#.####.##.####.#",   // 9
    "#......##......#",   // 10
    "###.##.##.##.###",   // 11
    "#......##......#",   // 12
    "#.####.##.####.#",   // 13
    "#..............#",   // 14
    "################",   // 15
};

// (cell-center X, cell-center Y) in normalized [0, 1] coords.
float cellCenter(int c) {
    return (static_cast<float>(c) + 0.5f) /
           static_cast<float>(MunchersGameState::kPelletGridSide);
}

// Bounding-box check: would a quad at (px, py) with half-size `half`
// overlap any wall cell?
bool overlapsWall(const std::array<std::uint64_t, 4>& walls,
                   float px, float py, float half) {
    const int x0 = positionToCell(px - half);
    const int x1 = positionToCell(px + half);
    const int y0 = positionToCell(py - half);
    const int y1 = positionToCell(py + half);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            if (cellGet(walls, cx, cy)) return true;
        }
    }
    return false;
}

void initMuncherStateIfNeeded(MunchersGameState& s) {
    if (s.initialized) return;

    // Populate wall bits from the ASCII maze.
    s.wallBits = {0ull, 0ull, 0ull, 0ull};
    for (int cy = 0; cy < MunchersGameState::kPelletGridSide; ++cy) {
        for (int cx = 0; cx < MunchersGameState::kPelletGridSide; ++cx) {
            if (kMaze[cy][cx] == '#') cellSet(s.wallBits, cx, cy);
        }
    }

    // Pellets fill every walkable cell (i.e. not a wall).
    s.pelletBits = {0ull, 0ull, 0ull, 0ull};
    for (int cy = 0; cy < MunchersGameState::kPelletGridSide; ++cy) {
        for (int cx = 0; cx < MunchersGameState::kPelletGridSide; ++cx) {
            if (kMaze[cy][cx] == '.') cellSet(s.pelletBits, cx, cy);
        }
    }

    // Snap Muncher to a known walkable cell — center of the bottom
    // corridor (row 14, column 8).
    s.muncherX = cellCenter(8);
    s.muncherY = cellCenter(14);

    // Spawn ghosts on the top corridor (row 3) so they have to traverse
    // the maze to reach the player.
    s.ghosts[0] = MunchersGameState::Ghost{cellCenter(2),  cellCenter(3), 0.0f, 0.0f};
    s.ghosts[1] = MunchersGameState::Ghost{cellCenter(8),  cellCenter(3), 0.0f, 0.0f};
    s.ghosts[2] = MunchersGameState::Ghost{cellCenter(13), cellCenter(3), 0.0f, 0.0f};

    s.initialized = true;
}

// Try the four cardinals in priority order (toward-player axes first) and
// return the first that the ghost can actually enter. Returns (vx, vy)
// scaled by speed, or (0, 0) if every direction is blocked.
void chooseGhostDirection(const std::array<std::uint64_t, 4>& walls,
                           const MunchersGameState::Ghost& g,
                           float playerX, float playerY,
                           float speedPerTick, float halfSize,
                           float& outVX, float& outVY) {
    const float dx = playerX - g.x;
    const float dy = playerY - g.y;
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);

    // Build a 4-entry candidate list in greedy-toward-player order.
    struct Cand { float vx, vy; };
    Cand candidates[4];
    if (ax >= ay) {
        // Prefer X-axis approach, then Y, then opposite X, then opposite Y.
        const float sxPrimary  = (dx > 0.0f) ?  1.0f : -1.0f;
        const float sxOpposite = -sxPrimary;
        const float syPrimary  = (dy > 0.0f) ?  1.0f : -1.0f;
        const float syOpposite = -syPrimary;
        candidates[0] = { sxPrimary  * speedPerTick, 0.0f };
        candidates[1] = { 0.0f, syPrimary  * speedPerTick };
        candidates[2] = { sxOpposite * speedPerTick, 0.0f };
        candidates[3] = { 0.0f, syOpposite * speedPerTick };
    } else {
        const float syPrimary  = (dy > 0.0f) ?  1.0f : -1.0f;
        const float syOpposite = -syPrimary;
        const float sxPrimary  = (dx > 0.0f) ?  1.0f : -1.0f;
        const float sxOpposite = -sxPrimary;
        candidates[0] = { 0.0f, syPrimary  * speedPerTick };
        candidates[1] = { sxPrimary  * speedPerTick, 0.0f };
        candidates[2] = { 0.0f, syOpposite * speedPerTick };
        candidates[3] = { sxOpposite * speedPerTick, 0.0f };
    }
    for (const auto& c : candidates) {
        const float tx = g.x + c.vx;
        const float ty = g.y + c.vy;
        if (!overlapsWall(walls, tx, ty, halfSize)) {
            outVX = c.vx;
            outVY = c.vy;
            return;
        }
    }
    outVX = 0.0f;
    outVY = 0.0f;
}

void tickGhost(MunchersGameState::Ghost& g,
                const std::array<std::uint64_t, 4>& walls,
                float playerX, float playerY,
                std::uint64_t simFrame, int ghostIndex,
                float speedPerTick, float halfSize) {
    // Re-pick a cardinal heading every kGhostDecisionTicks, staggered per
    // ghost so they don't change direction in lockstep. Heading is the
    // cardinal direction closest to the player vector, biased away from
    // walls.
    const std::uint64_t cadence = MunchersGameState::kGhostDecisionTicks;
    const bool needRepick =
        ((simFrame + static_cast<std::uint64_t>(ghostIndex) * 13ull) % cadence == 0) ||
        (g.vx == 0.0f && g.vy == 0.0f) ||
        overlapsWall(walls, g.x + g.vx, g.y + g.vy, halfSize);
    if (needRepick) {
        chooseGhostDirection(walls, g, playerX, playerY,
                              speedPerTick, halfSize, g.vx, g.vy);
    }
    const float tx = std::clamp(g.x + g.vx, 0.0f, 1.0f);
    const float ty = std::clamp(g.y + g.vy, 0.0f, 1.0f);
    if (!overlapsWall(walls, tx, ty, halfSize)) {
        g.x = tx;
        g.y = ty;
    } else {
        // Blocked even though we just picked — zero velocity so next
        // tick repicks via the (g.vx == 0 && g.vy == 0) clause above.
        g.vx = 0.0f;
        g.vy = 0.0f;
    }
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

    // Wall collision: try each axis independently so the player slides
    // along corridors when commanding into a wall corner. Half-size
    // here must match the Muncher's render half (kMuncherHalfPlay below
    // in GenerativeSystem) — kept slightly under one cell so the player
    // fits in a corridor.
    constexpr float kMuncherHalfPlay = 0.022f;
    const float tx = std::clamp(s.muncherX + s.muncherVelX, 0.0f, 1.0f);
    if (!overlapsWall(s.wallBits, tx, s.muncherY, kMuncherHalfPlay)) {
        s.muncherX = tx;
    } else {
        s.muncherVelX = 0.0f;
    }
    const float ty = std::clamp(s.muncherY + s.muncherVelY, 0.0f, 1.0f);
    if (!overlapsWall(s.wallBits, s.muncherX, ty, kMuncherHalfPlay)) {
        s.muncherY = ty;
    } else {
        s.muncherVelY = 0.0f;
    }

    // Eat any pellet under the player's current cell. Tiny score++ per
    // pellet for now; classic Pac-Man scores 10 per dot, but the
    // absolute number is meaningless without a score display.
    const int playerCellX = positionToCell(s.muncherX);
    const int playerCellY = positionToCell(s.muncherY);
    if (cellGet(s.pelletBits, playerCellX, playerCellY)) {
        cellClear(s.pelletBits, playerCellX, playerCellY);
        if (s.score < UINT16_MAX) ++s.score;
    }
    // Respawn pellets on every walkable cell when the grid empties.
    // Future: bump ghost speed each level.
    if (bitsEmpty(s.pelletBits)) {
        for (int cy = 0; cy < MunchersGameState::kPelletGridSide; ++cy) {
            for (int cx = 0; cx < MunchersGameState::kPelletGridSide; ++cx) {
                if (!cellGet(s.wallBits, cx, cy)) cellSet(s.pelletBits, cx, cy);
            }
        }
    }

    // Ghost half-size used by collision against walls (small enough to
    // fit within one cell). Must match the renderer's kGhostHalf.
    constexpr float kGhostHalfPlay = 0.020f;

    // Move ghosts and check collision against the player. On hit,
    // recenter the Muncher to the home tile + start a short invincibility
    // window so a clump of ghosts can't instant-respawn-kill.
    constexpr float kCollisionDist = 1.0f / MunchersGameState::kPelletGridSide * 0.6f;
    for (int gi = 0; gi < MunchersGameState::kNumGhosts; ++gi) {
        auto& g = s.ghosts[gi];
        tickGhost(g, s.wallBits, s.muncherX, s.muncherY,
                  s.simFrame, gi, s.ghostSpeedPerTick, kGhostHalfPlay);
        const float dx = g.x - s.muncherX;
        const float dy = g.y - s.muncherY;
        const float dist2 = dx * dx + dy * dy;
        const bool stillInvincible =
            s.lastHitFrame != 0 &&
            (s.simFrame - s.lastHitFrame) < MunchersGameState::kInvincibilityTicks;
        if (!stillInvincible && dist2 < kCollisionDist * kCollisionDist) {
            s.muncherX = cellCenter(8);
            s.muncherY = cellCenter(14);
            s.muncherVelX = 0.0f;
            s.muncherVelY = 0.0f;
            if (s.lives > 0) --s.lives;
            // Full reset when no lives left: refill walkable cells with
            // pellets, reset score, grant 3 lives, send ghosts back to
            // the top corridor.
            if (s.lives == 0) {
                for (int cy = 0; cy < MunchersGameState::kPelletGridSide; ++cy) {
                    for (int cx = 0; cx < MunchersGameState::kPelletGridSide; ++cx) {
                        if (!cellGet(s.wallBits, cx, cy)) cellSet(s.pelletBits, cx, cy);
                    }
                }
                s.score = 0;
                s.lives = 3;
                s.ghosts[0] = MunchersGameState::Ghost{cellCenter(2),  cellCenter(3), 0.0f, 0.0f};
                s.ghosts[1] = MunchersGameState::Ghost{cellCenter(8),  cellCenter(3), 0.0f, 0.0f};
                s.ghosts[2] = MunchersGameState::Ghost{cellCenter(13), cellCenter(3), 0.0f, 0.0f};
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
