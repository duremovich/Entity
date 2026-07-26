#include "entity/effects/EffectKindRegistry.hpp"

#include "entity/render/RuntimeShaderCompiler.hpp"

#include <nlohmann/json.hpp>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace entity::effects {

namespace {

using nlohmann::json;

ParamValue::Type paramTypeFromString(const std::string& s) {
    if (s == "Float") return ParamValue::Type::Float;
    if (s == "Int")   return ParamValue::Type::Int;
    if (s == "Vec2")  return ParamValue::Type::Vec2;
    if (s == "Vec3")  return ParamValue::Type::Vec3;
    if (s == "Color") return ParamValue::Type::Color;
    if (s == "Bool")  return ParamValue::Type::Bool;
    if (s == "Enum")  return ParamValue::Type::Enum;
    return ParamValue::Type::Float;
}

// Parse one ParamSchema entry from JSON. v1 only supports Float — other
// types are accepted at parse time but the UI won't expose them yet.
std::optional<ParamSchema> parseParamSchemaJson(const json& j) {
    if (!j.is_object() || !j.contains("name")) return std::nullopt;
    ParamSchema p;
    p.name        = j.at("name").get<std::string>();
    p.displayName = j.value("displayName", p.name);
    p.type        = paramTypeFromString(j.value("type", std::string{"Float"}));
    p.min         = j.value("min",  0.0f);
    p.max         = j.value("max",  1.0f);
    p.uiHint      = j.value("uiHint", 0);
    switch (p.type) {
        case ParamValue::Type::Float:
            p.defaultValue = ParamValue::makeFloat(j.value("default", 0.0f));
            break;
        case ParamValue::Type::Int:
        case ParamValue::Type::Enum:
            p.defaultValue = ParamValue::makeInt(j.value("default", 0));
            break;
        case ParamValue::Type::Bool:
            p.defaultValue = ParamValue::makeBool(j.value("default", false));
            break;
        default:
            // Vec2 / Vec3 / Color initialise to schema-default zero;
            // manifest authoring beyond Float arrives with the UI for
            // those types.
            p.defaultValue = {};
            p.defaultValue.type = p.type;
            break;
    }
    return p;
}

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

ParamSchema makeColorSchema(std::string name, std::string displayName,
                             float r, float g, float b, float a) {
    ParamSchema p;
    p.name         = std::move(name);
    p.displayName  = std::move(displayName);
    p.type         = ParamValue::Type::Color;
    p.defaultValue = ParamValue::makeColor(r, g, b, a);
    p.min          = 0.0f;
    p.max          = 1.0f;
    p.uiHint       = 1;  // color picker
    return p;
}

ParamSchema makeEnumSchema(std::string name, std::string displayName,
                            std::vector<std::string> labels, int defaultIdx) {
    ParamSchema p;
    p.name         = std::move(name);
    p.displayName  = std::move(displayName);
    p.type         = ParamValue::Type::Enum;
    p.defaultValue = ParamValue::makeEnum(defaultIdx);
    p.min          = 0.0f;
    p.max          = static_cast<float>(labels.empty() ? 0 : labels.size() - 1);
    p.uiHint       = 2;  // dropdown
    p.enumLabels   = std::move(labels);
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

// Generator variant: zero texture-input sockets — the kind's role marker
// (EffectKind::textureInputCount() == 0). The PS never samples g_input;
// PASS 1.5 may run a generator-led chain with no source texture at all.
EffectKind makeGeneratorKind(const char* stableId,
                              const char* displayName,
                              const char* psoFilename,
                              std::vector<ParamSchema> params)
{
    EffectKind k = makeBuiltinKind(stableId, displayName, "Generate",
                                   psoFilename, std::move(params));
    k.sockets = { textureOutput("out") };
    return k;
}

// Combiner variant: two texture inputs (a = t0, b = t1). In a linear
// stack the second input is unconnected (black fallback); the graph
// editor wires branches into it.
EffectKind makeCombinerKind(const char* stableId,
                             const char* displayName,
                             const char* psoFilename,
                             const char* secondInputName,
                             std::vector<ParamSchema> params)
{
    EffectKind k = makeBuiltinKind(stableId, displayName, "Combine",
                                   psoFilename, std::move(params));
    k.sockets = { textureInput("a"), textureInput(secondInputName),
                  textureOutput("out") };
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

    registerKind(makeBuiltinKind(
        "core.vignette",
        "Vignette",
        "Stylize",
        "vignette_ps.cso",
        {
            makeFloatSchema("radius",    "Radius",    0.6f, 0.0f, 1.0f),
            makeFloatSchema("softness",  "Softness",  0.3f, 0.0f, 1.0f),
            makeFloatSchema("intensity", "Intensity", 0.5f, 0.0f, 1.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.pixelate",
        "Pixelate",
        "Stylize",
        "pixelate_ps.cso",
        {
            makeFloatSchema("pixel_size", "Pixel Size", 8.0f, 1.0f, 128.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.sharpen",
        "Sharpen",
        "Stylize",
        "sharpen_ps.cso",
        {
            makeFloatSchema("amount", "Amount", 0.5f, 0.0f, 4.0f),
            makeFloatSchema("radius", "Radius (px)", 1.0f, 0.1f, 4.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.chromatic_aberration",
        "Chromatic Aberration",
        "Stylize",
        "chromatic_aberration_ps.cso",
        {
            makeFloatSchema("amount", "Amount (px)", 4.0f, -32.0f, 32.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.edge",
        "Edge Detect",
        "Stylize",
        "edge_ps.cso",
        {
            makeFloatSchema("threshold", "Threshold", 0.1f, 0.0f, 1.0f),
            makeFloatSchema("thickness", "Thickness (px)", 1.0f, 0.5f, 4.0f),
        }
    ));

    registerKind(makeBuiltinKind(
        "core.invert",
        "Invert",
        "Color",
        "invert_ps.cso",
        {
            makeFloatSchema("amount", "Amount", 1.0f, 0.0f, 1.0f),
        }
    ));

    // ---- Generators (zero texture inputs — replace the layer's content;
    // the AE model: drop one on a Solid). ----

    registerKind(makeGeneratorKind(
        "core.gen.linear_gradient",
        "Linear Gradient",
        "gen_linear_gradient_ps.cso",
        {
            makeFloatSchema("angle", "Angle (deg)", 0.0f, -180.0f, 180.0f),
            makeColorSchema("colorA", "Color A", 0.0f, 0.0f, 0.0f, 1.0f),
            makeColorSchema("colorB", "Color B", 1.0f, 1.0f, 1.0f, 1.0f),
        }
    ));

    registerKind(makeGeneratorKind(
        "core.gen.checkerboard",
        "Checkerboard",
        "gen_checkerboard_ps.cso",
        {
            makeFloatSchema("cols", "Columns", 8.0f, 1.0f, 128.0f),
            makeFloatSchema("rows", "Rows",    8.0f, 1.0f, 128.0f),
            makeColorSchema("colorA", "Color A", 0.0f, 0.0f, 0.0f, 1.0f),
            makeColorSchema("colorB", "Color B", 1.0f, 1.0f, 1.0f, 1.0f),
        }
    ));

    registerKind(makeGeneratorKind(
        "core.gen.fractal_noise",
        "Fractal Noise",
        "gen_fractal_noise_ps.cso",
        {
            makeFloatSchema("scale",    "Scale",    8.0f, 0.1f, 64.0f),
            makeFloatSchema("octaves",  "Octaves",  4.0f, 1.0f, 8.0f),
            makeFloatSchema("speed",    "Speed",    0.0f, 0.0f, 4.0f),
            makeFloatSchema("contrast", "Contrast", 1.0f, 0.0f, 4.0f),
        }
    ));

    registerKind(makeGeneratorKind(
        "core.gen.plasma",
        "Plasma",
        "gen_plasma_ps.cso",
        {
            makeFloatSchema("scale", "Scale", 8.0f, 0.1f, 64.0f),
            makeFloatSchema("speed", "Speed", 0.0f, 0.0f, 4.0f),
            makeColorSchema("colorA", "Color A", 0.0f, 0.0f, 0.2f, 1.0f),
            makeColorSchema("colorB", "Color B", 1.0f, 0.4f, 0.0f, 1.0f),
        }
    ));

    registerKind(makeGeneratorKind(
        "core.gen.shape",
        "Shape",
        "gen_shape_ps.cso",
        {
            makeEnumSchema("shape", "Shape", {"Circle", "Rectangle", "Ring"}, 0),
            makeFloatSchema("size",     "Size",     0.5f, 0.0f, 1.0f),
            makeFloatSchema("softness", "Softness", 0.02f, 0.0f, 0.5f),
            makeColorSchema("color", "Color", 1.0f, 1.0f, 1.0f, 1.0f),
        }
    ));

    // ---- Combiners (two texture inputs — the graph editor wires a
    // second branch into socket "b" / "mask" / "map"). ----

    registerKind(makeCombinerKind(
        "core.comb.blend",
        "Blend",
        "comb_blend_ps.cso",
        "b",
        {
            makeEnumSchema("mode", "Mode",
                           {"Normal", "Add", "Multiply", "Screen",
                            "Overlay", "Difference"}, 0),
            makeFloatSchema("opacity", "Opacity", 1.0f, 0.0f, 1.0f),
        }
    ));

    registerKind(makeCombinerKind(
        "core.comb.mask",
        "Mask",
        "comb_mask_ps.cso",
        "mask",
        {
            makeEnumSchema("channel", "Channel",
                           {"Luma", "Alpha", "R", "G", "B"}, 0),
            makeEnumSchema("invert", "Invert", {"Off", "On"}, 0),
            makeFloatSchema("softness", "Softness", 0.0f, 0.0f, 1.0f),
        }
    ));

    registerKind(makeCombinerKind(
        "core.comb.displace",
        "Displace",
        "comb_displace_ps.cso",
        "map",
        {
            makeFloatSchema("amountX", "Amount X (px)", 16.0f, -256.0f, 256.0f),
            makeFloatSchema("amountY", "Amount Y (px)", 16.0f, -256.0f, 256.0f),
        }
    ));
}

void EffectKindRegistry::scanUserEffects(const std::filesystem::path& projectEffectsDir,
                                          RuntimeShaderCompiler&       compiler,
                                          const std::filesystem::path& shaderIncludeDir) {
    namespace fs = std::filesystem;

    // Remember the arguments so hotReload() can re-run the scan.
    m_lastEffectsDir = projectEffectsDir;
    m_lastIncludeDir = shaderIncludeDir;
    m_lastCompiler   = &compiler;
    m_lastScanTouchedKinds.clear();
    m_compileErrors.clear();

    // Snapshot the existing user kinds instead of dropping them up
    // front: a kind whose recompile FAILS below must keep its last-good
    // registration + bytecode (and must NOT land in the touched list,
    // or the PSO invalidation would kill the running effect) — an
    // operator saving a syntax error mid-show keeps yesterday's shader
    // on the output, with the error surfaced in the UI. Kinds whose
    // manifests vanished entirely are removed at the end.
    std::unordered_map<std::uint32_t, EffectKind> previousUserKinds;
    {
        std::lock_guard<std::mutex> lk(m_kindsMutex);
        for (auto it = m_kinds.begin(); it != m_kinds.end(); ) {
            if (!it->second.builtin) {
                previousUserKinds.emplace(it->first, std::move(it->second));
                it = m_kinds.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Hashes seen in this scan (registered fresh OR kept as last-good).
    std::vector<std::uint32_t> liveUserKinds;

    std::error_code ec;
    if (!fs::exists(projectEffectsDir, ec) || !fs::is_directory(projectEffectsDir, ec)) {
        return;  // no user effects directory — silent no-op
    }

    // Include path passed to DXC for `#include "_effect_common.hlsli"`.
    // Engine-shipped header lives next to the executable's shaders/effects/
    // directory; callers pass that path so user HLSL can use the shared
    // cbuffer layout. Falls back to empty (DXC searches the source file's
    // directory by default) when unspecified.
    std::vector<std::wstring> includeDirs;
    if (!shaderIncludeDir.empty()) {
        includeDirs.push_back(shaderIncludeDir.wstring());
        // Also add the effects/ subdir explicitly so #include resolves the
        // header whether it's referenced as `_effect_common.hlsli` or
        // `effects/_effect_common.hlsli`.
        const fs::path subdir = shaderIncludeDir / "effects";
        if (fs::exists(subdir, ec)) {
            includeDirs.push_back(subdir.wstring());
        }
    }

    int scanned = 0;
    int registered = 0;
    for (const auto& entry : fs::directory_iterator(projectEffectsDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;
        ++scanned;

        json manifest;
        try {
            std::ifstream f(entry.path());
            manifest = json::parse(f);
        } catch (const std::exception& e) {
            std::cerr << "[scanUserEffects] Manifest parse failed: "
                      << entry.path().string() << " (" << e.what() << ")"
                      << std::endl;
            continue;
        }

        if (!manifest.contains("stableId") || !manifest.contains("shader")) {
            std::cerr << "[scanUserEffects] Manifest missing required field "
                         "(stableId / shader): "
                      << entry.path().string() << std::endl;
            continue;
        }

        const auto stableId   = manifest.value("stableId", std::string{});
        const auto displayName = manifest.value("displayName", stableId);
        const auto category   = manifest.value("category",  std::string{"User"});
        const auto shaderRel  = manifest.value("shader",    std::string{});
        const auto entryPoint = manifest.value("entryPoint", std::string{"PSMain"});
        const int  passCount  = manifest.value("passCount", 1);

        const fs::path shaderPath = projectEffectsDir / shaderRel;
        if (!fs::exists(shaderPath, ec)) {
            std::cerr << "[scanUserEffects] Shader not found: "
                      << shaderPath.string() << " (referenced by "
                      << entry.path().string() << ")" << std::endl;
            continue;
        }

        std::string source;
        try {
            std::ifstream f(shaderPath, std::ios::in | std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            source = ss.str();
        } catch (const std::exception& e) {
            std::cerr << "[scanUserEffects] Failed to read shader: "
                      << shaderPath.string() << " (" << e.what() << ")"
                      << std::endl;
            continue;
        }

        auto result = compiler.compile(source, entryPoint, "ps_6_0", includeDirs);
        if (!result.success || !result.blob) {
            std::cerr << "[scanUserEffects] HLSL compile failed for "
                      << stableId << ":\n" << result.errors << std::endl;
            // Surface in the UI (red badge on the stack header) and KEEP
            // the last-good registration + bytecode + PSO: the hash is
            // deliberately not added to the touched list, so no
            // InvalidateEffectPso fires and the running effect renders
            // yesterday's shader until the error is fixed.
            const std::uint32_t failedHash = fnv1a32(stableId);
            m_compileErrors[failedHash] = result.errors;
            auto prevIt = previousUserKinds.find(failedHash);
            if (prevIt != previousUserKinds.end()) {
                std::lock_guard<std::mutex> lk(m_kindsMutex);
                m_kinds.insert_or_assign(failedHash, std::move(prevIt->second));
                previousUserKinds.erase(prevIt);
                liveUserKinds.push_back(failedHash);
            }
            continue;
        }

        EffectKind kind;
        kind.stableId    = stableId;
        kind.kindIdHash  = fnv1a32(stableId);
        kind.displayName = displayName;
        kind.category    = category;
        kind.backend     = EffectKind::Backend::HLSLPixel;
        kind.shaderPath  = shaderPath.string();  // informational
        kind.passCount   = passCount;
        kind.builtin     = false;
        if (manifest.contains("params") && manifest["params"].is_array()) {
            for (const auto& pj : manifest["params"]) {
                if (auto ps = parseParamSchemaJson(pj)) {
                    kind.params.push_back(std::move(*ps));
                }
            }
        }
        // v1 socket schema: one TextureInput + one TextureOutput. Custom
        // sockets arrive with the node graph editor (Phase 4).
        kind.sockets = {
            { "in",  SocketSchema::Kind::Texture, SocketSchema::Direction::Input },
            { "out", SocketSchema::Kind::Texture, SocketSchema::Direction::Output },
        };

        // Snapshot bytecode into an owned, immutable byte vector so the
        // IDxcBlob can be dropped and the show thread can hold the
        // shared_ptr across PSO creation while hot reload swaps the map.
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(
            result.blob->GetBufferPointer());
        const std::size_t bytesLen = result.blob->GetBufferSize();
        auto artifact = std::make_shared<const std::vector<std::uint8_t>>(
            bytes, bytes + bytesLen);
        {
            std::lock_guard<std::mutex> lk(m_userArtifactsMutex);
            m_userArtifacts[kind.kindIdHash] = std::move(artifact);
        }
        m_lastScanTouchedKinds.push_back(kind.kindIdHash);
        liveUserKinds.push_back(kind.kindIdHash);

        registerKind(std::move(kind));
        ++registered;
    }

    // Kinds whose manifests vanished entirely: gone for real. Their
    // artifacts drop and their stale PSOs get invalidated.
    for (auto& [hash, kind] : previousUserKinds) {
        if (std::find(liveUserKinds.begin(), liveUserKinds.end(), hash)
            != liveUserKinds.end()) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(m_userArtifactsMutex);
            m_userArtifacts.erase(hash);
        }
        m_lastScanTouchedKinds.push_back(hash);
    }

    // Dedup touched hashes (a kind both dropped and re-registered
    // appears twice).
    std::sort(m_lastScanTouchedKinds.begin(), m_lastScanTouchedKinds.end());
    m_lastScanTouchedKinds.erase(
        std::unique(m_lastScanTouchedKinds.begin(), m_lastScanTouchedKinds.end()),
        m_lastScanTouchedKinds.end());

    if (scanned > 0) {
        std::cout << "[scanUserEffects] " << projectEffectsDir.string()
                  << ": scanned " << scanned << " manifest(s), registered "
                  << registered << " kind(s)" << std::endl;
    }
}

void EffectKindRegistry::hotReload() {
    if (!m_lastCompiler || m_lastEffectsDir.empty()) return;
    scanUserEffects(m_lastEffectsDir, *m_lastCompiler, m_lastIncludeDir);
}

void EffectKindRegistry::registerKind(EffectKind kind) {
    std::lock_guard<std::mutex> lk(m_kindsMutex);
    m_kinds.insert_or_assign(kind.kindIdHash, std::move(kind));
}

} // namespace entity::effects
