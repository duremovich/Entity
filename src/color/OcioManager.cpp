#include "entity/color/OcioManager.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <iostream>

namespace entity {

namespace OCIO = OCIO_NAMESPACE;

namespace {
// Preferred built-in config — the full ACES 1.3 Studio set with the v2.x
// view transforms baked in. OCIO ships this in the binary since 2.4; no
// filesystem deployment is required. If a future OCIO version drops or
// renames this exact config, initialize() falls back to whatever the
// library considers its default builtin via BuiltinConfigRegistry.
constexpr const char* kPreferredBuiltinConfig =
    "studio-config-v2.1.0_aces-v1.3_ocio-v2.3";
} // namespace

OcioManager::OcioManager() = default;
OcioManager::~OcioManager() = default;

bool OcioManager::initialize(const std::string& configPath) {
    m_isBuiltin  = false;
    m_isFallback = false;
    m_source.clear();

    if (configPath.empty()) {
        // Try the named studio config first.
        try {
            m_config = OCIO::Config::CreateFromBuiltinConfig(kPreferredBuiltinConfig);
            if (m_config) {
                m_isBuiltin = true;
                m_source    = std::string("builtin:") + kPreferredBuiltinConfig;
                return true;
            }
        } catch (const OCIO::Exception& e) {
            std::cerr << "[OcioManager] preferred built-in '"
                      << kPreferredBuiltinConfig << "' not registered: "
                      << e.what() << '\n';
        }

        // Fall back to the first "recommended" config the registry advertises.
        // OCIO 2.5's BuiltinConfigRegistry has no explicit "default" accessor;
        // recommended configs are the supported set.
        try {
            const auto& reg = OCIO::BuiltinConfigRegistry::Get();
            const size_t n  = reg.getNumBuiltinConfigs();
            for (size_t i = 0; i < n; ++i) {
                const char* name = reg.getBuiltinConfigName(i);
                if (!name || !*name) continue;
                if (!reg.isBuiltinConfigRecommended(i)) continue;
                try {
                    m_config = OCIO::Config::CreateFromBuiltinConfig(name);
                    if (m_config) {
                        m_isBuiltin = true;
                        m_source    = std::string("builtin:") + name;
                        std::cerr << "[OcioManager] using fallback builtin '"
                                  << name << "' (preferred '"
                                  << kPreferredBuiltinConfig
                                  << "' not available)\n";
                        return true;
                    }
                } catch (const OCIO::Exception&) {
                    continue;
                }
            }
        } catch (const OCIO::Exception& e) {
            std::cerr << "[OcioManager] builtin registry lookup failed: "
                      << e.what() << '\n';
        }
    } else {
        try {
            m_config = OCIO::Config::CreateFromFile(configPath.c_str());
            if (m_config) {
                m_source = configPath;
                return true;
            }
        } catch (const OCIO::Exception& e) {
            std::cerr << "[OcioManager] failed to load OCIO config '"
                      << configPath << "': " << e.what() << '\n';
        } catch (const std::exception& e) {
            std::cerr << "[OcioManager] unexpected error loading OCIO config '"
                      << configPath << "': " << e.what() << '\n';
        }
    }

    // Identity-config fallback. Editor still launches; user sees no color
    // transforms applied until they fix the path.
    try {
        m_config = OCIO::Config::CreateRaw();
    } catch (const OCIO::Exception& e) {
        std::cerr << "[OcioManager] CreateRaw failed (very unexpected): "
                  << e.what() << '\n';
    }
    m_isFallback = true;
    m_source     = "fallback:identity";
    return false;
}

std::vector<std::string> OcioManager::listColorSpaces() const {
    std::vector<std::string> out;
    if (!m_config) return out;
    int n = m_config->getNumColorSpaces();
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const char* name = m_config->getColorSpaceNameByIndex(i);
        if (name) out.emplace_back(name);
    }
    return out;
}

std::vector<std::string> OcioManager::listDisplays() const {
    std::vector<std::string> out;
    if (!m_config) return out;
    int n = m_config->getNumDisplays();
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const char* d = m_config->getDisplay(i);
        if (d) out.emplace_back(d);
    }
    return out;
}

std::vector<std::string> OcioManager::listViews(const std::string& display) const {
    std::vector<std::string> out;
    if (!m_config) return out;
    int n = m_config->getNumViews(display.c_str());
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const char* v = m_config->getView(display.c_str(), i);
        if (v) out.emplace_back(v);
    }
    return out;
}

std::string OcioManager::getDefaultDisplay() const {
    if (!m_config) return {};
    const char* d = m_config->getDefaultDisplay();
    return d ? std::string(d) : std::string();
}

std::string OcioManager::getDefaultView(const std::string& display) const {
    if (!m_config) return {};
    const char* v = m_config->getDefaultView(display.c_str());
    return v ? std::string(v) : std::string();
}

} // namespace entity
