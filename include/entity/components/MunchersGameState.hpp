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
    // [Layer::startFrame, Layer::startFrame + Layer::duration)). V1 skeleton
    // uses this as a "the layer is alive" pulse; once show-side rendering
    // exists it will hash to a hue for the placeholder color-cycling output.
    std::uint64_t simFrame{0};

    // TODO(muncher v2): full simulation state. Sketch:
    //   float    muncherX, muncherY;          // tile-fractional position
    //   uint8_t  muncherDir;                  // 0=up 1=right 2=down 3=left
    //   uint8_t  muncherQueuedDir;
    //   struct Ghost { float x, y; uint8_t dir, mode; uint16_t frightenedFramesLeft; };
    //   Ghost    ghosts[3];
    //   uint8_t  pelletsBits[28 * 31 / 8];    // bitset over maze tiles
    //   uint16_t score;
    //   uint8_t  lives;
    //   uint8_t  level;
    //   uint8_t  phase;                       // Start / Playing / Paused / Death / Win
    //   uint32_t phaseFramesLeft;
    //
    // Plus tunable / externally-driven parameters (think disguise Tennis's
    // "ball speed in pixels per beat" + "left/right bat position"):
    //   float    speedTilesPerSecond;
    //   float    frightenedSeconds;
    //   float    ghostSpeedMultiplier;
    //   uint8_t  playerDirInput;              // OSC / MIDI / keyboard writes here
    //   bool     resetPulse;
};

} // namespace entity
