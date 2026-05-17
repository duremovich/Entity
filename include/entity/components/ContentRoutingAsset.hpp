#pragma once

#include "entity/components/ContentRouting.hpp"  // RouteMode, RouteTarget

#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace entity {

/**
 * ContentRoutingAsset — a named, reusable Plane A routing (ADR-0022).
 *
 * Lives on a standalone library entity (no Layer, no Transform). Multiple
 * Clip / GenerativeLayer entities can reference one asset via
 * ContentRoutingRef. The editor-thread snapshot bake resolves the ref
 * before populating `bus::ContentLayerSnapshot.routes` — the show thread
 * sees baked routes only and has no awareness of assets.
 *
 * Authoring kinds (FeedMap added in L3 of ADR-0022):
 *   Direct  — Single target screen, identity uvRect. Auto-generated per
 *             Screen by RoutingLibrarySystem; one auto-direct asset per
 *             Screen, name-synced to the Screen until the user manually
 *             diverges it.
 *   Tiled   — Multi-target with derived uvRects from (tiledCount,
 *             tiledAxis). `targets` holds the materialized uvRects for
 *             the snapshot bake.
 *   FeedMap — (L3) Multi-target with explicit per-region uvRect + name.
 *             Named regions of an authored canvas; `sourceWidth/Height`
 *             carry the source-canvas metadata used by visualization and
 *             template export.
 *
 * Soft-rule exception: carries std::string + std::vector. Library assets
 * are NOT iterated in any per-frame view, so the cost is irrelevant.
 * Documented exception alongside Clip's RAII destructor and Transform's
 * inline math.
 */
struct ContentRoutingAsset {
    std::string name;
    RouteMode kind{RouteMode::Direct};
    std::vector<RouteTarget> targets;

    // Tiled-only authoring metadata. Ignored when kind != Tiled.
    // `targets` is the materialized truth; (tiledCount, tiledAxis) is
    // wizard state used to re-derive uvRects when either changes.
    std::uint8_t tiledCount{1};
    std::uint8_t tiledAxis{0};   // 0 = horizontal, 1 = vertical

    // FeedMap-only authoring metadata. Ignored when kind != FeedMap.
    // Source canvas resolution that the named regions are laid out
    // against. Used by L4 visualization + L3 template export.
    std::uint32_t sourceWidth{1920};
    std::uint32_t sourceHeight{1080};

    // Auto-direct lifecycle binding. Non-null means this asset was
    // auto-generated for the bound Screen by RoutingLibrarySystem. On
    // Screen destroy the system cascade-deletes this asset and clears
    // any ContentRoutingRef::asset pointing at it. User-created assets
    // carry entt::null here.
    entt::entity autoBoundScreen{entt::null};

    // Last Screen name we synced this asset's `name` to. RoutingLibrary-
    // System reads it each tick: if `name == lastSyncedScreenName`, the
    // asset is still in autosync mode and a Screen rename also renames
    // the asset; otherwise the user has manually renamed and autosync
    // is broken. Only meaningful when `autoBoundScreen != null`.
    std::string lastSyncedScreenName;
};

} // namespace entity
