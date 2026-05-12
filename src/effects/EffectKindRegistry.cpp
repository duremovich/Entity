#include "entity/effects/EffectKindRegistry.hpp"

namespace entity::effects {

// Phase 1 stubs. The wire types, snapshot bake, and PASS 1.5 plumbing
// are exercised against an empty registry. Phase 2 populates built-ins
// (gaussian_blur / brightness_contrast / hue_saturation), Phase 3 rounds
// out to ~10 effects, Phase 6 wires user scanning + hot reload.

void EffectKindRegistry::registerBuiltins() {
    // Phase 2: register engine effects here.
}

void EffectKindRegistry::scanUserEffects(const std::filesystem::path& /*projectEffectsDir*/) {
    // Phase 6: read manifests + compile HLSL via RuntimeShaderCompiler.
}

void EffectKindRegistry::hotReload(const std::filesystem::path& /*changedFile*/) {
    // Phase 6: re-scan the single changed file.
}

void EffectKindRegistry::registerKind(EffectKind kind) {
    m_kinds.insert_or_assign(kind.kindIdHash, std::move(kind));
}

} // namespace entity::effects
