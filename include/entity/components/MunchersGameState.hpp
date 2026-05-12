#pragma once

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

    // TODO(muncher v3): ghosts, pellet grid, score / lives / phase. Sketch:
    //   struct Ghost { float x, y, vx, vy; uint8_t mode; uint16_t frightenedFramesLeft; };
    //   Ghost    ghosts[3];
    //   uint8_t  pelletsBits[16 * 16 / 8];   // small bitset over maze tiles
    //   uint16_t score;
    //   uint8_t  lives;
    //   uint8_t  phase;                       // Start / Playing / Death / Win
};

} // namespace entity
