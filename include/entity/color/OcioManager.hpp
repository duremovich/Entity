#pragma once

#include <OpenColorIO/OpenColorTypes.h>

#include <string>
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

private:
    OCIO_NAMESPACE::ConstConfigRcPtr m_config;
    std::string m_source;
    bool m_isBuiltin{false};
    bool m_isFallback{false};
};

} // namespace entity
