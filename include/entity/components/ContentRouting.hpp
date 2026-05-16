#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <entt/entt.hpp>

namespace entity {

/**
 * Content routing mode (Plane A per ADR-0021).
 *
 * Direct  — Single target screen, identity uvRect. Equivalent to the legacy
 *           Clip.targetScreen / GenerativeLayer.targetScreen behavior, with
 *           an empty targets vector preserving the old "render on all
 *           visible screens" semantic.
 *
 * Tiled   — Multi-screen feed map (M3). Multiple targets, each with its own
 *           uvRect crop of the source UV. One content source carved across
 *           N screens.
 *
 * Cylindrical, Spherical, Perspective — Reserved, deferred to a future ADR.
 *           Will attach kind-specific sidecar components per the
 *           composition rule (ADR-0016) rather than fattening this enum's
 *           payload struct.
 */
enum class RouteMode : uint8_t {
    Direct = 0,
    Tiled  = 1,
};

/**
 * One destination inside a ContentRouting (Plane A per ADR-0021).
 *
 * Identifies a target screen and the UV-space rectangle of the source
 * content that lands on it. uvRect = {x, y, w, h} where (0,0,1,1) means
 * the full source frame. The layer's UV-space Transform (ADR-0018)
 * composes on top of uvRect at PASS 2 draw time.
 */
struct RouteTarget {
    entt::entity screen{entt::null};
    std::array<float, 4> uvRect{0.0f, 0.0f, 1.0f, 1.0f};
};

/**
 * ContentRouting — how a content-producing layer (Clip, GenerativeLayer,
 * future kinds) lands on one or more screens (Plane A per ADR-0021).
 *
 * Empty `targets` is the legacy "no targetScreen / render on all visible
 * screens" semantic. PASS 2 of CompositorSystem expands the empty-targets
 * case to one synthetic route per visible screen at snapshot-bake time so
 * the show thread sees an explicit list.
 *
 * Replaces the single-entity Clip.targetScreen field as the source of
 * truth in Phase M2. `Clip.targetScreen` and `GenerativeLayer.targetScreen`
 * remain as deprecated alias fields for one project-format version (read
 * on load, kept in sync on save) and are dropped in Phase M4.
 *
 * Lives on the content-producing layer entity (the Clip / GenerativeLayer
 * entity itself), never on the target screen.
 */
struct ContentRouting {
    RouteMode mode{RouteMode::Direct};
    std::vector<RouteTarget> targets;
};

} // namespace entity
