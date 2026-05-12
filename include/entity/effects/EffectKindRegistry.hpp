#pragma once

#include "EffectKind.hpp"

#include <cstdint>
#include <filesystem>
#include <unordered_map>

namespace entity::effects {

// EffectKindRegistry — owns the catalog of available effect kinds.
//
// Engine effects (~10 starter set) are registered statically via
// `registerBuiltins`. User effects are scanned at project-open via
// `scanUserEffects` reading HLSL + JSON manifest pairs from a
// project-local directory; the runtime-DXC compile path arrives in a
// later phase.
//
// The registry is editor-thread-owned. The show thread never consults
// it directly — `PlaybackTimeAuthority::buildSceneSnapshot` resolves
// each effect's kind on the editor thread and bakes the resolved
// schema-driven paramBlob into `bus::EffectSnapshot`. PSO caches on
// the renderer side key off `kindIdHash` and load shader artifacts
// from `EffectKind::shaderPath` on first use.
//
// Phase 1 (skeleton): the public surface exists but `registerBuiltins`
// and `scanUserEffects` are no-ops. Phase 2 populates built-ins.
// Phase 6 wires user scanning + hot reload.
class EffectKindRegistry {
public:
    EffectKindRegistry() = default;
    ~EffectKindRegistry() = default;

    EffectKindRegistry(const EffectKindRegistry&) = delete;
    EffectKindRegistry& operator=(const EffectKindRegistry&) = delete;

    // Registers the engine-shipped effect catalog. No-op in Phase 1.
    void registerBuiltins();

    // Scans a project's effects/ directory for user HLSL + manifest
    // pairs. No-op in Phase 1; full implementation in Phase 6.
    void scanUserEffects(const std::filesystem::path& projectEffectsDir);

    // Re-scan a single changed file. Hot-reload entry point for the
    // ContentScanner integration in Phase 6.
    void hotReload(const std::filesystem::path& changedFile);

    // Lookup by hash. nullptr if no kind with this hash exists.
    const EffectKind* find(std::uint32_t kindIdHash) const noexcept {
        auto it = m_kinds.find(kindIdHash);
        return (it == m_kinds.end()) ? nullptr : &it->second;
    }

    // Direct add — used by registerBuiltins and the future user-scan
    // path. Overwrites any existing kind with the same hash (silent;
    // hot-reload depends on this).
    void registerKind(EffectKind kind);

    // Read-only access for the Add Effect menu / node graph picker.
    const std::unordered_map<std::uint32_t, EffectKind>& kinds() const noexcept {
        return m_kinds;
    }

private:
    std::unordered_map<std::uint32_t, EffectKind> m_kinds;
};

} // namespace entity::effects
