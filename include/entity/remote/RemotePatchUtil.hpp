#pragma once

#include "entity/components/Clip.hpp"
#include "entity/components/MunchersGameState.hpp"
#include "entity/components/RemotePatch.hpp"
#include "entity/components/TextLayerState.hpp"

#include <entt/entt.hpp>

#include <string>
#include <string_view>

namespace entity::remote {

// Charset contract for patch ids: non-empty, [a-z0-9_] only.
inline bool isValidPatchId(std::string_view id) {
    if (id.empty()) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                        || c == '_';
        if (!ok) return false;
    }
    return true;
}

inline bool patchIdInUse(const entt::registry& registry,
                         std::string_view id) {
    for (auto [e, rp] : registry.view<const RemotePatch>().each()) {
        if (rp.patchId == id) return true;
    }
    return false;
}

// Kind-derived base: video / muncher / text / layer.
inline const char* autoPatchBase(const entt::registry& registry,
                                 entt::entity layerEntity) {
    if (registry.all_of<TextLayerState>(layerEntity))    return "text";
    if (registry.all_of<MunchersGameState>(layerEntity)) return "muncher";
    if (registry.all_of<Clip>(layerEntity))              return "video";
    return "layer";
}

// First free "<base><n>" starting at 1.
inline std::string makeAutoPatchId(const entt::registry& registry,
                                   entt::entity layerEntity) {
    const std::string base = autoPatchBase(registry, layerEntity);
    for (int n = 1; ; ++n) {
        std::string candidate = base + std::to_string(n);
        if (!patchIdInUse(registry, candidate)) return candidate;
    }
}

} // namespace entity::remote
