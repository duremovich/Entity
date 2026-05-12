#pragma once

#include "entity/core/Types.hpp"

#include <entt/entt.hpp>

#include <cstdint>

namespace entity {

/**
 * GenerativeLayer component — marks a Layer entity as a procedural texture
 * source (e.g. the "Muncher" Pac-Man-style mini-game, or future kinds like
 * particles, audio visualizers, etc.).
 *
 * The layer entity also carries:
 *   Layer            — timeline placement (startFrame, duration, trackIndex,
 *                      kind == Kind::Generative)
 *   MediaLayer       — opacity / zOrder / blendMode for the produced texture
 *   <kind-specific>  — e.g. MunchersGameState for the Muncher kind. Sub-kind
 *                      is signaled by which state component is attached;
 *                      same composition-over-Kind-enum rule used to
 *                      distinguish Clip vs ObjectAnimation today
 *                      (ADR-0016).
 *
 * GenerativeSystem ticks the per-tick simulation on the editor thread.
 * The show thread (Phase 2) will read a baked GenerativeLayerSnapshot and
 * render the procedural content into renderTargetSlot before
 * CompositorSystem composes it onto targetScreen.
 *
 * ADR-0014: written only on the editor thread.
 */
struct GenerativeLayer {
    // Routing — generative layers produce a texture and route it to a Screen,
    // mirroring Clip::targetScreen. entt::null means "first available" (the
    // CreateGenerativeLayerCommand assigns one at creation time).
    entt::entity  targetScreen{entt::null};

    // Output render-target resolution. The show thread allocates a render
    // target of this size on first use and posts the slot back to the editor
    // via an R2D ack (Phase 2 wiring — same pattern as Screen compose
    // targets). renderTargetSlot stays -1 until the ack lands.
    std::uint32_t renderWidth{1920};
    std::uint32_t renderHeight{1080};
    int           renderTargetSlot{-1};
};

} // namespace entity
