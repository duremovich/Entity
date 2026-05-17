#pragma once

#include "entity/systems/System.hpp"

#include <entt/entt.hpp>

namespace entity {

/**
 * RoutingLibrarySystem — owner of the ContentRoutingAsset lifecycle
 * (ADR-0022).
 *
 * Auto-direct rule: every Screen gets a `ContentRoutingAsset` of
 * `kind = Direct` automatically. Per tick, this system reconciles the
 * registry:
 *
 *   - For each Screen without a bound auto-direct asset: emplace one
 *     (Direct, single target = the Screen, identity uvRect, name =
 *     Screen::name). Manually-deleted auto-direct assets reappear on
 *     the next tick this way.
 *
 *   - For each ContentRoutingAsset with `autoBoundScreen != null`:
 *       - Bound Screen destroyed → delete asset, null out every
 *         `ContentRoutingRef::asset` that pointed at it.
 *       - Screen renamed AND asset is still in autosync mode
 *         (`asset.name == lastSyncedScreenName`) → rename the asset
 *         and bump `lastSyncedScreenName`. If the user has diverged
 *         the asset name manually, just bump `lastSyncedScreenName`
 *         silently so future Screen renames evaluate against the
 *         current Screen name.
 *
 * Editor-thread only. ADR-0014: writes the registry, never reads from
 * the show thread. The bake site
 * (PlaybackTimeAuthority::buildSceneSnapshot) resolves
 * ContentRoutingRef → asset every snapshot, so reconcile runs on the
 * editor thread before each bake.
 */
class RoutingLibrarySystem : public System {
public:
    RoutingLibrarySystem() = default;

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "RoutingLibrarySystem"; }

    // Run one reconcile pass immediately. Used by project load to
    // materialize auto-direct assets after Screen entities have been
    // constructed, before any snapshot bake reads the library.
    void reconcileNow(entt::registry& registry);
};

} // namespace entity
