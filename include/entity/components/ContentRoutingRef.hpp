#pragma once

#include <entt/entt.hpp>

namespace entity {

/**
 * ContentRoutingRef — Clip / GenerativeLayer-side pointer at the
 * ContentRoutingAsset that drives this layer's screen routing (ADR-0022).
 *
 * `asset == entt::null` is the "render on all visible screens" semantic,
 * equivalent to the pre-ADR-0022 inline `ContentRouting` with an empty
 * `targets` vector.
 *
 * Snapshot bake (PlaybackTimeAuthority::buildSceneSnapshot) resolves the
 * ref through the registry, copies the asset's routes into
 * `bus::ContentLayerSnapshot.routes`, and writes asset.kind into `mode`.
 * The show thread sees no asset/ref distinction — the wire format from
 * ADR-0021 is preserved.
 *
 * If `asset` references a destroyed entity (e.g. a v19 project loaded
 * before auto-direct assets are materialized), the bake treats it as
 * null and emits a one-time warning.
 */
struct ContentRoutingRef {
    entt::entity asset{entt::null};
};

} // namespace entity
