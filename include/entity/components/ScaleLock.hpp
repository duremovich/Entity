#pragma once

namespace entity {

/**
 * Per-entity "uniform scale" lock: when set, editing any one scale axis
 * scales the others by the same ratio.
 *
 * This used to be a UI-only `std::unordered_map<entt::entity, bool>` inside
 * PropertyWindow, which meant (a) the timeline's twirl-down scale rows had no
 * way to see it — editing Scale X there broke the lock — and (b) it reset on
 * every project load. As a component both editors read the same flag and it
 * persists.
 *
 * Absent component == locked. `true` is the historical default, so projects
 * saved before this component existed load with the lock on, as they did before.
 */
struct ScaleLock {
    bool uniform{true};
};

}  // namespace entity
