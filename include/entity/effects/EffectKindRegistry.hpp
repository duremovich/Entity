#pragma once

#include "EffectKind.hpp"

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace entity {
class RuntimeShaderCompiler;
}

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

    // Scan a project's effects/ directory for user HLSL + manifest
    // pairs. Each `<name>.json` manifest declares a kind; the matching
    // `<name>.hlsl` (or the file named in manifest's "shader" field) is
    // compiled in-process via the passed RuntimeShaderCompiler. Failed
    // compiles are logged and skipped (the kind doesn't appear in the
    // registry). Previously-scanned user kinds are dropped before
    // re-scanning so the call is idempotent.
    //
    // Phase 6 ships with scan-on-project-load. Hot-reload via
    // ContentScanner is a follow-up.
    void scanUserEffects(const std::filesystem::path& projectEffectsDir,
                          RuntimeShaderCompiler&       compiler,
                          const std::filesystem::path& shaderIncludeDir = {});

    // Re-scan a single changed file. Hot-reload entry point for the
    // ContentScanner integration follow-up.
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

    // View of a user effect's compiled PS bytecode. Renderer prefers this
    // over loading a .cso from disk when present. Returns size = 0 when
    // there's no in-memory bytecode for this kind (engine effects always
    // fall back to the disk path).
    struct PsBytecodeView {
        const std::uint8_t* data{nullptr};
        std::size_t         size{0};
        bool valid() const { return data != nullptr && size > 0; }
    };
    PsBytecodeView tryGetUserPsBytecode(std::uint32_t kindIdHash) const noexcept {
        auto it = m_userArtifacts.find(kindIdHash);
        if (it == m_userArtifacts.end()) return {};
        return { it->second.psBytecode.data(), it->second.psBytecode.size() };
    }

private:
    std::unordered_map<std::uint32_t, EffectKind> m_kinds;

    // Compiled-bytecode storage for user-authored kinds. Keyed by kind
    // hash. Engine kinds aren't in here — they load `.cso` from disk on
    // first reference.
    struct UserArtifact {
        std::vector<std::uint8_t> psBytecode;
    };
    std::unordered_map<std::uint32_t, UserArtifact> m_userArtifacts;
};

} // namespace entity::effects
