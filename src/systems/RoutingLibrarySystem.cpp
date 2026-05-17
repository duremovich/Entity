#include "entity/systems/RoutingLibrarySystem.hpp"

#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Screen.hpp"
#include "entity/profile/Tracy.hpp"

#include <iostream>

namespace entity {

namespace {

// Build a fresh auto-direct asset for a given Screen.
void emplaceAutoDirectAsset(entt::registry& registry,
                             entt::entity screenEntity,
                             const Screen& screen) {
    auto assetEntity = registry.create();
    auto& asset = registry.emplace<ContentRoutingAsset>(assetEntity);
    asset.name = screen.name;
    asset.kind = RouteMode::Direct;
    RouteTarget t;
    t.screen = screenEntity;
    t.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
    asset.targets.push_back(t);
    asset.autoBoundScreen = screenEntity;
    asset.lastSyncedScreenName = screen.name;
}

// Walk every ContentRoutingRef in the registry and null any that point
// at `assetEntity`. Used on cascade-delete of an auto-direct asset
// after its bound Screen is destroyed.
void clearRefsTo(entt::registry& registry, entt::entity assetEntity) {
    auto refView = registry.view<ContentRoutingRef>();
    for (auto [refEntity, ref] : refView.each()) {
        if (ref.asset == assetEntity) {
            ref.asset = entt::null;
        }
    }
}

void reconcileImpl(entt::registry& registry) {
    ZoneScopedN("RoutingLibrarySystem::reconcile");

    // Collect Screens that currently have an auto-direct asset bound to
    // them. Done in one pass to avoid O(N*M) lookups.
    std::vector<entt::entity> screensWithAutoDirect;
    screensWithAutoDirect.reserve(8);

    // Pass 1: walk auto-bound assets. Cascade-delete orphans; sync names
    // on still-bound ones.
    std::vector<entt::entity> assetsToDestroy;
    {
        auto view = registry.view<ContentRoutingAsset>();
        for (auto [assetEntity, asset] : view.each()) {
            if (asset.autoBoundScreen == entt::null) continue;

            if (!registry.valid(asset.autoBoundScreen) ||
                !registry.all_of<Screen>(asset.autoBoundScreen)) {
                // Bound Screen gone — cascade-delete asset, clear refs.
                clearRefsTo(registry, assetEntity);
                assetsToDestroy.push_back(assetEntity);
                continue;
            }

            const auto& screen = registry.get<Screen>(asset.autoBoundScreen);
            if (screen.name != asset.lastSyncedScreenName) {
                if (asset.name == asset.lastSyncedScreenName) {
                    // Autosync still in effect: rename the asset along
                    // with the Screen.
                    asset.name = screen.name;
                }
                // Else: user has manually renamed the asset; just bump
                // lastSyncedScreenName silently so future Screen renames
                // are evaluated against the current Screen name.
                asset.lastSyncedScreenName = screen.name;
            }

            // Keep the bound Screen pinned as the first target of an
            // auto-direct asset that's still Direct. If the user has
            // promoted it to Tiled / changed targets manually, leave the
            // targets alone — `autoBoundScreen` is purely a lifecycle
            // marker at that point.
            if (asset.kind == RouteMode::Direct &&
                asset.targets.size() == 1 &&
                asset.targets[0].screen != asset.autoBoundScreen) {
                asset.targets[0].screen = asset.autoBoundScreen;
            }

            screensWithAutoDirect.push_back(asset.autoBoundScreen);
        }
    }

    for (auto e : assetsToDestroy) {
        std::cout << "[RoutingLibrarySystem] cascade-deleting auto-direct asset for destroyed Screen"
                  << std::endl;
        registry.destroy(e);
    }

    // Pass 2: find Screens that have no auto-direct asset and create one.
    auto screenView = registry.view<Screen>();
    for (auto [screenEntity, screen] : screenView.each()) {
        bool found = false;
        for (auto e : screensWithAutoDirect) {
            if (e == screenEntity) {
                found = true;
                break;
            }
        }
        if (!found) {
            emplaceAutoDirectAsset(registry, screenEntity, screen);
        }
    }
}

} // namespace

void RoutingLibrarySystem::initialize(entt::registry& registry) {
    reconcileImpl(registry);
}

void RoutingLibrarySystem::update(entt::registry& registry, float /*deltaTime*/) {
    ZoneScopedN("RoutingLibrarySystem");
    reconcileImpl(registry);
}

void RoutingLibrarySystem::shutdown(entt::registry& /*registry*/) {
    // No external resources to release.
}

void RoutingLibrarySystem::reconcileNow(entt::registry& registry) {
    reconcileImpl(registry);
}

} // namespace entity
