#include "entity/effects/EffectKindRegistry.hpp"

namespace entity::effects {

namespace {

// Shared helpers to keep registerBuiltins below readable.
ParamSchema makeFloatSchema(std::string name, std::string displayName,
                             float defaultVal, float minVal, float maxVal) {
    ParamSchema p;
    p.name         = std::move(name);
    p.displayName  = std::move(displayName);
    p.type         = ParamValue::Type::Float;
    p.defaultValue = ParamValue::makeFloat(defaultVal);
    p.min          = minVal;
    p.max          = maxVal;
    p.uiHint       = 0;  // slider
    return p;
}

SocketSchema textureInput(const char* name) {
    SocketSchema s;
    s.name      = name;
    s.kind      = SocketSchema::Kind::Texture;
    s.direction = SocketSchema::Direction::Input;
    return s;
}

SocketSchema textureOutput(const char* name) {
    SocketSchema s;
    s.name      = name;
    s.kind      = SocketSchema::Kind::Texture;
    s.direction = SocketSchema::Direction::Output;
    return s;
}

EffectKind makeBuiltinKind(const char* stableId,
                            const char* displayName,
                            const char* category,
                            const char* psoFilename,
                            std::vector<ParamSchema> params)
{
    EffectKind k;
    k.stableId    = stableId;
    k.kindIdHash  = fnv1a32(k.stableId);
    k.displayName = displayName;
    k.category    = category;
    k.params      = std::move(params);
    k.sockets     = { textureInput("in"), textureOutput("out") };
    k.backend     = EffectKind::Backend::HLSLPixel;
    k.shaderPath  = std::string("shaders/") + psoFilename;
    k.passCount   = 1;
    k.builtin     = true;
    return k;
}

} // namespace

void EffectKindRegistry::registerBuiltins() {
    // Three starter engine effects validating the PASS 1.5 plumbing.
    // Schemas are positional — slot N maps to g_params[N].x in the
    // shared effect cbuffer (see shaders/effects/_effect_common.hlsli).
    // Phase 3 rounds the catalog out to ~10 effects.

    registerKind(makeBuiltinKind(
        "core.brightness_contrast",
        "Brightness & Contrast",
        "Color",
        "brightness_contrast_ps.cso",
        {
            makeFloatSchema("brightness", "Brightness", 0.0f, -1.0f, 1.0f),
            makeFloatSchema("contrast",   "Contrast",   0.0f, -1.0f, 1.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.gaussian_blur",
        "Gaussian Blur",
        "Stylize",
        "gaussian_blur_ps.cso",
        {
            makeFloatSchema("radius", "Radius (px)", 0.0f, 0.0f, 64.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.hue_saturation",
        "Hue / Saturation",
        "Color",
        "hue_saturation_ps.cso",
        {
            makeFloatSchema("hue",        "Hue (deg)",  0.0f, -180.0f, 180.0f),
            makeFloatSchema("saturation", "Saturation", 0.0f,   -1.0f,   1.0f),
            makeFloatSchema("lightness",  "Lightness",  0.0f,   -1.0f,   1.0f),
        }
    ));
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
