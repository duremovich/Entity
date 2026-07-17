#pragma once

#include "EffectKind.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

    // Hot-reload entry point, driven by ContentScanner EffectSource
    // deltas (Engine::drainContentScannerDeltas). Re-runs the full
    // scanUserEffects pass with the arguments stored by the last scan —
    // the effects dir is a handful of files, so a targeted single-
    // manifest recompile isn't worth the manifest-to-shader reverse
    // mapping. No-op if no scan has run yet.
    void hotReload();

    // Hashes of user kinds whose PSOs must be invalidated after the last
    // scanUserEffects / hotReload call: every user kind that existed
    // before the scan plus every one registered by it (a removed kind's
    // stale PSO must go too). The editor sends one InvalidateEffectPso
    // D2R message per hash.
    const std::vector<std::uint32_t>& lastScanTouchedKinds() const noexcept {
        return m_lastScanTouchedKinds;
    }

    // Compile diagnostics from the last scan, keyed by kind hash
    // (fnv1a32 of the manifest's stableId). Present = the kind failed to
    // compile and kept no artifact; UI surfaces the message. Editor
    // thread only.
    const std::unordered_map<std::uint32_t, std::string>& compileErrors() const noexcept {
        return m_compileErrors;
    }

    // Lookup by hash. nullptr if no kind with this hash exists.
    // EDITOR THREAD ONLY: hot reload mutates m_kinds on the editor
    // thread, so the returned pointer (and the map walk itself) is only
    // safe from that thread. The show thread must use findCopy().
    const EffectKind* find(std::uint32_t kindIdHash) const noexcept {
        auto it = m_kinds.find(kindIdHash);
        return (it == m_kinds.end()) ? nullptr : &it->second;
    }

    // Show-thread-safe lookup: copies the kind under the registry lock
    // so hot reload's erase/re-insert (editor thread) can never leave
    // the caller holding a dangling pointer or walking a rehashing map.
    // Copy cost (strings + schema vectors) only lands on the PSO-cache
    // miss path — cold by construction.
    std::optional<EffectKind> findCopy(std::uint32_t kindIdHash) const {
        std::lock_guard<std::mutex> lk(m_kindsMutex);
        auto it = m_kinds.find(kindIdHash);
        if (it == m_kinds.end()) return std::nullopt;
        return it->second;
    }

    // Direct add — used by registerBuiltins and the future user-scan
    // path. Overwrites any existing kind with the same hash (silent;
    // hot-reload depends on this).
    void registerKind(EffectKind kind);

    // Read-only access for the Add Effect menu / node graph picker.
    const std::unordered_map<std::uint32_t, EffectKind>& kinds() const noexcept {
        return m_kinds;
    }

    // A user effect's compiled PS bytecode, or nullptr when there's no
    // in-memory artifact for this kind (engine effects always fall back
    // to the disk .cso path). Returned as a shared_ptr the caller holds
    // across PSO creation: hot reload swaps the map entry on the editor
    // thread while the show thread builds PSOs on cache miss, so the
    // published vector is immutable and the pointer keeps it alive.
    std::shared_ptr<const std::vector<std::uint8_t>>
    tryGetUserPsBytecode(std::uint32_t kindIdHash) const {
        std::lock_guard<std::mutex> lk(m_userArtifactsMutex);
        auto it = m_userArtifacts.find(kindIdHash);
        return (it == m_userArtifacts.end()) ? nullptr : it->second;
    }

private:
    // Guards m_kinds for the one cross-thread reader (findCopy on the
    // show thread's PSO-miss path). All mutation (registerBuiltins /
    // scanUserEffects / hotReload) and the pointer-returning find() are
    // editor-thread-only; mutators take this lock so findCopy can't
    // observe an erase/rehash mid-walk.
    mutable std::mutex m_kindsMutex;
    std::unordered_map<std::uint32_t, EffectKind> m_kinds;

    // Compiled-bytecode storage for user-authored kinds. Keyed by kind
    // hash. Engine kinds aren't in here — they load `.cso` from disk on
    // first reference. Values are immutable once published (hot reload
    // replaces the shared_ptr, never mutates the vector); the mutex
    // covers the map itself for the editor-write / show-read race.
    mutable std::mutex m_userArtifactsMutex;
    std::unordered_map<std::uint32_t,
                       std::shared_ptr<const std::vector<std::uint8_t>>>
        m_userArtifacts;

    // Last-scan arguments so hotReload() can re-run without the caller
    // re-threading them. Compiler is engine-owned and outlives us.
    std::filesystem::path  m_lastEffectsDir;
    std::filesystem::path  m_lastIncludeDir;
    RuntimeShaderCompiler* m_lastCompiler{nullptr};

    std::vector<std::uint32_t>                     m_lastScanTouchedKinds;
    std::unordered_map<std::uint32_t, std::string> m_compileErrors;
};

} // namespace entity::effects
