#pragma once

#include <array>
#include <cstdint>

namespace entity {

/**
 * MunchersGameState — per-tick simulation state for the "Muncher" kind of
 * GenerativeLayer. The Muncher is the lower-case-"e" Entity logo as a
 * maze-chase player; the layer simulates a generic maze-chase game with
 * theme-swappable ghost sprites loaded from an asset pack at runtime.
 *
 * The presence of this component on a Layer+GenerativeLayer entity is what
 * tells GenerativeSystem to treat the layer as a Muncher layer — no Kind
 * enum dispatch (ADR-0016 composition rule).
 *
 * This file is the V1 SKELETON. Only the bookkeeping field `simFrame` is
 * populated by GenerativeSystem. The real simulation state — Muncher
 * position, ghost positions/AI, pellet grid, score, lives, phase — lands
 * in the next commit alongside show-side rendering and input plumbing.
 *
 * ADR-0014: written only on the editor thread.
 */
struct MunchersGameState {
    // Monotonic simulation tick counter, incremented once per editor tick
    // when the parent Layer is active (current frame in
    // [Layer::startFrame, Layer::startFrame + Layer::duration)).
    std::uint64_t simFrame{0};

    // --- Player ----------------------------------------------------------
    // Position in the layer's normalized output space [0, 1] × [0, 1].
    // Origin is top-left; Y grows downward (matches the show-side render
    // coordinate convention).
    float muncherX{0.5f};
    float muncherY{0.5f};

    // Per-tick velocity actually applied to muncherX/Y, after snapping the
    // input axes to a cardinal direction. Persisted so the player keeps
    // moving in the last commanded direction even after the axis returns to
    // zero — classic Pac-Man "continues until intersection / wall" behavior.
    // (No maze walls yet, so v1 just keeps moving until the input changes
    // or the axis returns to zero.)
    float muncherVelX{0.0f};
    float muncherVelY{0.0f};

    // --- External input axes --------------------------------------------
    // Last read from InputBus channels "muncher.input.x" / ".y", clamped to
    // [-1, 1]. Bakes into the snapshot so the show thread can render an
    // input indicator / debug overlay later if needed.
    //
    // Per the disguise Tennis precedent ("left bat position" / "right bat
    // position" are user-tweakable floats, driven externally by MIDI
    // faders), Muncher's controls are externally-drivable floats too. A
    // joystick maps naturally: stick X → muncher.input.x, stick Y →
    // muncher.input.y. For now scripts and the editor keyboard handler
    // write directly.
    float inputX{0.0f};
    float inputY{0.0f};

    // --- Tunables (per-layer config — surfaces in Properties panel later) ----
    // Movement speed in normalized units per editor tick. ~600 ticks per
    // screen traversal at the default. The headless editor runs at
    // thousands of fps with no decode, so this is intentionally tiny;
    // when we tie game tempo to wall-clock or beat time (Phase 4), this
    // becomes "units per second" or "units per beat" instead.
    float speedPerTick{0.005f};

    // Input deadzone: |axis| below this is treated as zero. Stops noise
    // from cheap analog sticks from creeping the player.
    float inputDeadzone{0.20f};

    // --- Ghosts ----------------------------------------------------------
    // Three chasers spawn from the corners (skipping the bottom-right,
    // which is where the Muncher starts moving toward by default). AI is
    // re-picked every kGhostDecisionTicks ticks, staggered across ghosts
    // so they don't move in lockstep. Future: per-ghost behavior modes
    // (scatter / chase / frightened) and a personality bias so each
    // ghost picks its target differently.
    struct Ghost {
        float x{0.5f};
        float y{0.5f};
        float vx{0.0f};
        float vy{0.0f};
    };
    static constexpr int kNumGhosts = 3;
    std::array<Ghost, kNumGhosts> ghosts{};

    // Ghost movement speed (slower than the player so the Muncher can
    // outrun them on a straightaway).
    float ghostSpeedPerTick{0.0030f};

    // Decision cadence — ghosts re-pick their cardinal heading every N
    // ticks. Stagger applied per-ghost in GenerativeSystem.
    static constexpr std::uint64_t kGhostDecisionTicks = 30;

    // --- Maze ------------------------------------------------------------
    // 16×16 grid of cells. Each cell is either a wall or walkable. Pellets
    // only spawn on walkable cells. Both Muncher and ghosts are blocked by
    // walls (axis-by-axis collision so they slide along corridors).
    //
    // The grid is the same shape as the pellet bitset — 256 bits packed
    // into 4 × uint64, same `cy * 16 + cx` index, low-bit-first. Bit set
    // = wall; bit clear = walkable.
    static constexpr int kPelletGridSide = 16;
    static constexpr int kPelletCells    = kPelletGridSide * kPelletGridSide;
    std::array<std::uint64_t, 4> wallBits{};

    // Pellets on walkable cells. Bit set = pellet present; bit clear =
    // already eaten. On full clear GenerativeSystem refills walkable
    // cells (next "level").
    std::array<std::uint64_t, 4> pelletBits{};

    // --- Score / lives / lifecycle ---------------------------------------
    std::uint16_t score{0};
    std::uint16_t lives{3};
    // Tick of the last ghost-Muncher collision. Used for a short
    // post-hit invincibility window so the Muncher isn't instantly
    // re-killed by overlapping ghosts.
    std::uint64_t lastHitFrame{0};
    static constexpr std::uint64_t kInvincibilityTicks = 60;

    // First-tick init guard. GenerativeSystem populates pelletBits +
    // ghost spawn positions on the first tick after the layer becomes
    // active. Keeps the component a default-initializable POD; no
    // constructor in createMuncherLayer needs to know game-state details.
    bool initialized{false};
};

} // namespace entity
