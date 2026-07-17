#pragma once

#include "entity/components/EffectParam.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace entity::effects {

// Constexpr FNV-1a 32-bit hash. Used to derive an EffectKind's stable
// numeric ID from its string ID (e.g. "core.blur" → 0x...). Wire
// serialization carries the string; the hash is the in-memory lookup
// key so PSO caches and snapshot bake stay branch-free.
constexpr std::uint32_t fnv1a32(const char* s, std::size_t n) {
    std::uint32_t h = 0x811c9dc5u;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint8_t>(s[i]);
        h *= 0x01000193u;
    }
    return h;
}

inline std::uint32_t fnv1a32(std::string_view s) {
    return fnv1a32(s.data(), s.size());
}

struct ParamSchema {
    std::string       name;         // wire identifier; e.g. "radius"
    std::string       displayName;  // UI label; e.g. "Radius"
    ParamValue::Type  type{ParamValue::Type::Float};
    ParamValue        defaultValue{};
    float             min{0.0f};
    float             max{1.0f};
    // 0=slider, 1=colorPicker, 2=dropdown, 3=checkbox, ...
    int               uiHint{0};
    // Enum dropdown option labels (only meaningful when type == Enum).
    std::vector<std::string> enumLabels;
};

struct SocketSchema {
    enum class Kind : std::uint8_t {
        Texture = 0,
        Float   = 1,
        Color   = 2,
        Vec2    = 3,
    };
    enum class Direction : std::uint8_t {
        Input  = 0,
        Output = 1,
    };

    std::string name;
    Kind        kind{Kind::Texture};
    Direction   direction{Direction::Input};
};

struct EffectKind {
    enum class Backend : std::uint8_t {
        HLSLPixel   = 0,
        HLSLCompute = 1,
    };

    std::uint32_t              kindIdHash{0};
    std::string                stableId;     // e.g. "core.blur"
    std::string                displayName;  // e.g. "Blur"
    std::string                category;     // e.g. "Stylize", "Color", "User"
    std::vector<ParamSchema>   params;
    std::vector<SocketSchema>  sockets;
    Backend                    backend{Backend::HLSLPixel};
    std::string                shaderPath;   // .cso (engine) or .hlsl (user)
    int                        passCount{1}; // >1 for separable passes etc.
    bool                       builtin{true};

    // Number of Texture-kind input sockets. The single source of truth
    // for a kind's role: 0 = generator (ignores/replaces its input —
    // noise, gradients), 1 = filter, >= 2 = combiner (blend/mask). No
    // separate isGenerator flag to drift out of sync with the sockets.
    int textureInputCount() const {
        int n = 0;
        for (const auto& s : sockets) {
            if (s.kind == SocketSchema::Kind::Texture &&
                s.direction == SocketSchema::Direction::Input) {
                ++n;
            }
        }
        return n;
    }
};

} // namespace entity::effects
