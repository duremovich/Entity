#pragma once

#include <cstdint>

namespace entity {

// One effect node in a layer's effect chain. Each effect is its own
// entity (effects-as-entities, ADR-0019 pending). The owning layer's
// EffectChain component holds the ordered list of effect entities and
// the graph topology. Per-effect parameter values + animated keyframe
// tracks live in sibling components (EffectParameters,
// EffectAnimatedParameters).
//
// `kindId` is the FNV-1a hash of the kind's stable string ID (e.g.
// FNV-1a("core.blur") for engine effects, FNV-1a("user.vignette") for
// project-local user-authored ones). The EffectKindRegistry maps
// kindId → ParamSchema + shader artefact.
//
// graphX / graphY are the node's position in the node graph editor.
// Stored on the component so they round-trip through save/load and
// undo/redo without a parallel UI-state map.
struct Effect {
    std::uint32_t kindId{0};
    bool          enabled{true};
    float         graphX{0.0f};
    float         graphY{0.0f};
};

} // namespace entity
