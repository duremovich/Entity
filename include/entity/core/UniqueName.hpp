#pragma once

/**
 * Default-name helper for stage objects (Screens, Props, Projectors).
 *
 * Project files refer to these objects by name (outputs' sourceScreenName,
 * projector targets, routing targets, layer targets, sourceProjectorName),
 * so two objects with the same name are ambiguous on reload. Every place
 * the editor invents a name ("+ Add Screen", "Duplicate") goes through
 * makeUniqueName so new objects never collide.
 */

#include <entt/entt.hpp>

#include <string>

namespace entity {

/// `base` if `exists(base)` is false, otherwise the first of "base 2",
/// "base 3", ... that is free.
template <typename ExistsFn>
std::string makeUniqueName(const std::string& base, ExistsFn&& exists) {
    if (!exists(base)) return base;
    for (int n = 2;; ++n) {
        std::string candidate = base + " " + std::to_string(n);
        if (!exists(candidate)) return candidate;
    }
}

/// makeUniqueName against the `name` field of every `Component` in `reg`.
template <typename Component>
std::string makeUniqueComponentName(entt::registry& reg, const std::string& base) {
    return makeUniqueName(base, [&](const std::string& candidate) {
        for (auto [e, c] : reg.view<Component>().each()) {
            if (c.name == candidate) return true;
        }
        return false;
    });
}

} // namespace entity
