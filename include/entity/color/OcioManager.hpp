#pragma once

#include "entity/color/OcioGpuProcessor.hpp"

#include <OpenColorIO/OpenColorTypes.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entity {

/**
 * OcioManager — owns the project's active OpenColorIO config.
 *
 * Phase C.12 entrypoint. Subtasks 1-3 only expose read-only accessors used
 * by the Settings + MappingWindow UIs; subtask 4 extends this class with
 * GPU processor caches that drive the renderer's input + display transforms.
 *
 * Lifecycle:
 *   - Construct (cheap, no OCIO calls).
 *   - initialize(path): empty path = built-in ACES Studio Config (the OCIO
 *     library ships it since 2.4 — no on-disk resource needed). Non-empty
 *     path = CreateFromFile. On failure falls back to CreateRaw (identity
 *     config) so the editor still launches with a non-null config.
 *   - Engine owns one instance and reloads it when Settings.ocioConfigPath
 *     changes (subtask 7). Hot-reload requires app restart in C.12.
 */
class OcioManager {
public:
    OcioManager();
    ~OcioManager();

    OcioManager(const OcioManager&) = delete;
    OcioManager& operator=(const OcioManager&) = delete;

    /**
     * Load the config. Returns true if the *intended* config (built-in or
     * file) loaded successfully; false if the manager had to fall back to an
     * identity config. Either way getConfig() returns a non-null pointer
     * after this call (the fallback config is a real OCIO::Config).
     */
    bool initialize(const std::string& configPath = {});

    OCIO_NAMESPACE::ConstConfigRcPtr getConfig() const { return m_config; }

    /** True if the active config came from CreateFromBuiltinConfig. */
    bool isUsingBuiltinConfig() const { return m_isBuiltin; }

    /** True if initialize() failed and we're running on an identity config. */
    bool isUsingFallback() const { return m_isFallback; }

    /** Name of the built-in or file path the config was loaded from. */
    const std::string& configSource() const { return m_source; }

    std::vector<std::string> listColorSpaces() const;
    std::vector<std::string> listDisplays() const;
    std::vector<std::string> listViews(const std::string& display) const;

    std::string getDefaultDisplay() const;
    std::string getDefaultView(const std::string& display) const;

    // -----------------------------------------------------------------
    // Phase C.12 #4 — GPU processor cache.
    //
    // buildInputProcessor   — transform from a clip's source color space
    //                         to the engine's working space (ACEScg).
    //                         Used by composite_ps in subtask 5.
    //
    // buildDisplayProcessor — transform from working space (ACEScg) out
    //                         to a display+view (e.g. sRGB / ACES SDR).
    //                         Used by mapping_surface_ps + the C.12 #3
    //                         capture pass.
    //
    // Both methods cache. Repeat calls with the same key return the same
    // shared_ptr (no recompile / no recodegen). Returns nullptr if the
    // requested transform doesn't exist in the active config (logs).
    // -----------------------------------------------------------------
    std::shared_ptr<const OcioGpuProcessor>
        buildInputProcessor(const std::string& srcColorSpace,
                            const std::string& workingSpace = "ACEScg");

    std::shared_ptr<const OcioGpuProcessor>
        buildDisplayProcessor(const std::string& workingSpace,
                              const std::string& display,
                              const std::string& view);

    /** Drop all cached processors. Called when the OCIO config is reloaded. */
    void clearProcessorCache();

    size_t inputCacheSize() const;
    size_t displayCacheSize() const;

private:
    // Build and populate an OcioGpuProcessor from an OCIO Processor +
    // function-name choice. Pulls textures + uniforms + HLSL out of OCIO's
    // GpuShaderDesc into the project value type.
    std::shared_ptr<const OcioGpuProcessor>
        bakeProcessor(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                      const std::string& functionName,
                      const std::string& resourcePrefix) const;

    OCIO_NAMESPACE::ConstConfigRcPtr m_config;
    std::string m_source;
    bool m_isBuiltin{false};
    bool m_isFallback{false};

    mutable std::mutex                                                   m_cacheMutex;
    std::unordered_map<std::string, std::shared_ptr<const OcioGpuProcessor>> m_inputCache;
    std::unordered_map<std::string, std::shared_ptr<const OcioGpuProcessor>> m_displayCache;
};

} // namespace entity
