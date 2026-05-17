#pragma once

#include "entity/components/ContentRouting.hpp"        // RouteMode, RouteTarget
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Screen.hpp"

#include <entt/entt.hpp>

#include <string>
#include <utility>
#include <vector>

namespace entity { namespace routing {

/**
 * Helpers for working with the ContentRoutingAsset library (ADR-0022).
 *
 * These exist so call sites — PropertyWindow, Commands, ProjectSerializer
 * migration — don't reimplement the same ref-creation logic three slightly
 * different ways. Trivial inline operations, no virtuals, no per-frame
 * iteration. They operate at the layer/library boundary (writing
 * ContentRoutingRef on a layer entity, creating ContentRoutingAsset
 * entities) so they're editor-thread-only by ADR-0014.
 */

// Find the auto-direct ContentRoutingAsset bound to a given Screen.
// Returns entt::null if no auto-direct asset exists yet (the next
// RoutingLibrarySystem reconcile will create one, but the caller might
// need it now — see setLayerTargetScreen).
inline entt::entity findAutoDirectAsset(const entt::registry& reg,
                                         entt::entity screen) {
    if (screen == entt::null) return entt::null;
    auto view = reg.view<ContentRoutingAsset>();
    for (auto [e, asset] : view.each()) {
        if (asset.autoBoundScreen == screen) return e;
    }
    return entt::null;
}

// Materialize an auto-direct asset for a Screen on demand (rather than
// waiting for the per-tick reconcile). Used when a UI command needs to
// produce a stable ContentRoutingRef before the next tick.
inline entt::entity ensureAutoDirectAsset(entt::registry& reg,
                                            entt::entity screen) {
    if (screen == entt::null) return entt::null;
    if (auto existing = findAutoDirectAsset(reg, screen); existing != entt::null) {
        return existing;
    }
    const auto* s = reg.try_get<Screen>(screen);
    auto assetEntity = reg.create();
    auto& asset = reg.emplace<ContentRoutingAsset>(assetEntity);
    asset.name = s ? s->name : "Unnamed Screen";
    asset.kind = RouteMode::Direct;
    RouteTarget t;
    t.screen = screen;
    asset.targets.push_back(t);
    asset.autoBoundScreen = screen;
    asset.lastSyncedScreenName = asset.name;
    return assetEntity;
}

// Set the layer's routing to "single screen" by pointing its
// ContentRoutingRef at the auto-direct asset for `screen`. Passing
// entt::null clears the ref (== "all visible screens").
inline void setLayerTargetScreen(entt::registry& reg,
                                   entt::entity layer,
                                   entt::entity screen) {
    auto& ref = reg.get_or_emplace<ContentRoutingRef>(layer);
    ref.asset = (screen == entt::null)
        ? entt::null
        : ensureAutoDirectAsset(reg, screen);
}

// Create a fresh user-named asset with the given mode + targets, and
// point the layer's ContentRoutingRef at it. Returns the new asset
// entity. Used when a layer needs a one-off custom routing that isn't
// the auto-direct of any single Screen (e.g. Tiled).
inline entt::entity setLayerCustomRouting(entt::registry& reg,
                                            entt::entity layer,
                                            RouteMode mode,
                                            std::vector<RouteTarget> targets,
                                            std::string preferredName = {}) {
    auto pickName = [&]() {
        if (!preferredName.empty()) {
            // Verify uniqueness against existing assets.
            bool taken = false;
            for (auto [e, a] : reg.view<ContentRoutingAsset>().each()) {
                if (a.name == preferredName) { taken = true; break; }
            }
            if (!taken) return preferredName;
        }
        int n = 1;
        std::string name;
        bool taken = true;
        while (taken) {
            name = "Custom Routing " + std::to_string(n);
            taken = false;
            for (auto [e, a] : reg.view<ContentRoutingAsset>().each()) {
                if (a.name == name) { taken = true; break; }
            }
            ++n;
        }
        return name;
    };

    auto assetEntity = reg.create();
    auto& asset = reg.emplace<ContentRoutingAsset>(assetEntity);
    asset.name = pickName();
    asset.kind = mode;
    asset.targets = std::move(targets);
    asset.autoBoundScreen = entt::null;

    auto& ref = reg.get_or_emplace<ContentRoutingRef>(layer);
    ref.asset = assetEntity;
    return assetEntity;
}

// Resolve a layer's ContentRoutingRef to its asset. Returns nullptr if
// the ref is missing, null, or points at a destroyed entity. Read-only;
// safe from anywhere on the editor thread.
inline const ContentRoutingAsset*
tryGetAsset(const entt::registry& reg, entt::entity layer) {
    const auto* ref = reg.try_get<ContentRoutingRef>(layer);
    if (!ref || ref->asset == entt::null) return nullptr;
    if (!reg.valid(ref->asset)) return nullptr;
    return reg.try_get<ContentRoutingAsset>(ref->asset);
}

}} // namespace entity::routing
