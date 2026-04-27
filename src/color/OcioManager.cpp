#include "entity/color/OcioManager.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
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

// =============================================================================
// Phase C.12 #4 — GPU processor cache + GpuShaderDesc → OcioGpuProcessor bake.
// =============================================================================

namespace {

constexpr const char* kFunctionInputPrefix   = "OCIO_input_";
constexpr const char* kFunctionDisplayPrefix = "OCIO_display_";

// Sanitize a name (color space / display / view) into a token suitable for
// HLSL identifiers + resource prefixes. ASCII-only — replace anything else
// with underscores. OCIO names like "Linear Rec.709 (sRGB)" become
// "Linear_Rec_709__sRGB_".
std::string mangleForHlsl(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::string makeInputCacheKey(const std::string& src, const std::string& dst) {
    return src + "->" + dst;
}

std::string makeDisplayCacheKey(const std::string& src,
                                const std::string& display,
                                const std::string& view) {
    return src + "->" + display + ":" + view;
}

} // namespace

std::shared_ptr<const OcioGpuProcessor>
OcioManager::bakeProcessor(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                           const std::string& functionName,
                           const std::string& resourcePrefix) const {
    if (!processor) return nullptr;

    auto out = std::make_shared<OcioGpuProcessor>();
    out->m_processor = processor;

    OCIO::GpuShaderDescRcPtr desc;
    try {
        desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_HLSL_DX11);
        desc->setFunctionName(functionName.c_str());
        desc->setResourcePrefix(resourcePrefix.c_str());
        // OCIO defaults to "outColor" for the per-pixel variable; we'll keep
        // that in the generated HLSL and rename via a wrapper at splice time
        // if the host shader uses something else.

        OCIO::ConstGPUProcessorRcPtr gpu = processor->getDefaultGPUProcessor();
        if (!gpu) {
            std::cerr << "[OcioManager] getDefaultGPUProcessor returned null\n";
            return nullptr;
        }
        gpu->extractGpuShaderInfo(desc);
    } catch (const OCIO::Exception& e) {
        std::cerr << "[OcioManager] GpuShaderDesc extraction failed: "
                  << e.what() << '\n';
        return nullptr;
    }

    // Read the *actually emitted* function name back from OCIO. setFunctionName
    // sanitizes the input (collapses consecutive underscores etc.) so the name
    // we passed in may not match what's in the generated HLSL. Subtask 5
    // splices on this canonical name when wiring the call site.
    if (const char* fn = desc->getFunctionName()) {
        out->m_functionName.assign(fn);
    } else {
        out->m_functionName = functionName;
    }

    // Stable copy of the shader text — getShaderText returns a pointer into
    // OCIO-owned memory that becomes invalid if the desc goes out of scope.
    const char* text = desc->getShaderText();
    out->m_shaderText.assign(text ? text : "");

    // 1D / 2D LUT textures.
    const unsigned numTextures = desc->getNumTextures();
    out->m_textures.reserve(static_cast<size_t>(numTextures + desc->getNum3DTextures()));
    for (unsigned i = 0; i < numTextures; ++i) {
        const char* tname = nullptr;
        const char* sname = nullptr;
        unsigned width    = 0;
        unsigned height   = 0;
        OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dims{};
        OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
        desc->getTexture(i, tname, sname, width, height, channel, dims, interp);

        OcioGpuProcessor::TextureBinding tb;
        tb.textureName  = tname ? tname : "";
        tb.samplerName  = sname ? sname : "";
        tb.width        = width;
        tb.height       = height;
        tb.depth        = 0;
        tb.isRgb        = (channel == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL);
        tb.interpolation = interp;
        tb.kind         = (dims == OCIO::GpuShaderDesc::TEXTURE_2D)
                            ? OcioGpuProcessor::TextureKind::Lut2D
                            : OcioGpuProcessor::TextureKind::Lut1D;

        const float* values = nullptr;
        desc->getTextureValues(i, values);
        if (values) {
            const size_t pixelCount = static_cast<size_t>(width)
                                    * static_cast<size_t>(std::max(height, 1u));
            const size_t channels   = tb.isRgb ? 3u : 1u;
            tb.values.assign(values, values + pixelCount * channels);
        }
        out->m_textures.push_back(std::move(tb));
    }

    // 3D LUT textures (always RGB).
    const unsigned num3D = desc->getNum3DTextures();
    for (unsigned i = 0; i < num3D; ++i) {
        const char* tname = nullptr;
        const char* sname = nullptr;
        unsigned edgelen  = 0;
        OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
        desc->get3DTexture(i, tname, sname, edgelen, interp);

        OcioGpuProcessor::TextureBinding tb;
        tb.textureName  = tname ? tname : "";
        tb.samplerName  = sname ? sname : "";
        tb.kind         = OcioGpuProcessor::TextureKind::Lut3D;
        tb.width        = edgelen;
        tb.height       = edgelen;
        tb.depth        = edgelen;
        tb.isRgb        = true;
        tb.interpolation = interp;

        const float* values = nullptr;
        desc->get3DTextureValues(i, values);
        if (values) {
            const size_t voxelCount = static_cast<size_t>(edgelen)
                                    * static_cast<size_t>(edgelen)
                                    * static_cast<size_t>(edgelen);
            tb.values.assign(values, values + voxelCount * 3u);
        }
        out->m_textures.push_back(std::move(tb));
    }

    // Uniforms.
    const unsigned numUniforms = desc->getNumUniforms();
    out->m_uniforms.reserve(numUniforms);
    for (unsigned i = 0; i < numUniforms; ++i) {
        OcioGpuProcessor::UniformBinding ub;
        const char* uname = desc->getUniform(i, ub.rawData);
        ub.name = uname ? uname : "";
        out->m_uniforms.push_back(std::move(ub));
    }

    return out;
}

std::shared_ptr<const OcioGpuProcessor>
OcioManager::buildInputProcessor(const std::string& srcColorSpace,
                                 const std::string& workingSpace) {
    if (!m_config) return nullptr;
    if (srcColorSpace.empty() || workingSpace.empty()) return nullptr;

    const std::string key = makeInputCacheKey(srcColorSpace, workingSpace);
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_inputCache.find(key);
        if (it != m_inputCache.end()) return it->second;
    }

    OCIO::ConstProcessorRcPtr processor;
    try {
        processor = m_config->getProcessor(srcColorSpace.c_str(), workingSpace.c_str());
    } catch (const OCIO::Exception& e) {
        std::cerr << "[OcioManager] input processor '" << srcColorSpace
                  << "' -> '" << workingSpace << "' not available: "
                  << e.what() << '\n';
        return nullptr;
    }
    if (!processor) return nullptr;

    const std::string mangled  = mangleForHlsl(srcColorSpace);
    const std::string fnName   = std::string(kFunctionInputPrefix)   + mangled;
    const std::string resPrefix = std::string("ocio_input_") + mangled + "_";
    auto baked = bakeProcessor(processor, fnName, resPrefix);
    if (!baked) return nullptr;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto [it, inserted] = m_inputCache.try_emplace(key, baked);
        if (!inserted) return it->second;
    }
    return baked;
}

std::shared_ptr<const OcioGpuProcessor>
OcioManager::buildDisplayProcessor(const std::string& workingSpace,
                                   const std::string& display,
                                   const std::string& view) {
    if (!m_config) return nullptr;
    if (workingSpace.empty() || display.empty() || view.empty()) return nullptr;

    const std::string key = makeDisplayCacheKey(workingSpace, display, view);
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_displayCache.find(key);
        if (it != m_displayCache.end()) return it->second;
    }

    OCIO::ConstProcessorRcPtr processor;
    try {
        processor = m_config->getProcessor(workingSpace.c_str(),
                                           display.c_str(),
                                           view.c_str(),
                                           OCIO::TRANSFORM_DIR_FORWARD);
    } catch (const OCIO::Exception& e) {
        std::cerr << "[OcioManager] display processor '" << workingSpace
                  << "' -> '" << display << "' / '" << view << "' not available: "
                  << e.what() << '\n';
        return nullptr;
    }
    if (!processor) return nullptr;

    const std::string mangled = mangleForHlsl(display + "_" + view);
    const std::string fnName  = std::string(kFunctionDisplayPrefix) + mangled;
    const std::string resPrefix = std::string("ocio_display_") + mangled + "_";
    auto baked = bakeProcessor(processor, fnName, resPrefix);
    if (!baked) return nullptr;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto [it, inserted] = m_displayCache.try_emplace(key, baked);
        if (!inserted) return it->second;
    }
    return baked;
}

void OcioManager::clearProcessorCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_inputCache.clear();
    m_displayCache.clear();
}

size_t OcioManager::inputCacheSize() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_inputCache.size();
}

size_t OcioManager::displayCacheSize() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_displayCache.size();
}

} // namespace entity
