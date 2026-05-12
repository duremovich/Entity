/**
 * Unit tests for EffectKindRegistry::scanUserEffects (Phase 6 of #54).
 *
 * Creates a temp directory with a manifest + HLSL pair, scans it via the
 * registry, and asserts the kind appears with compiled bytecode. Engine
 * effects (registerBuiltins) and project bake are not exercised here.
 */

#include <gtest/gtest.h>

#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/render/RuntimeShaderCompiler.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr const char* kPassthroughHlsl = R"(
// Trivial passthrough — no cbuffer / sampler use so it doesn't need the
// effect-common header. The scan still compiles + registers it.
struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(PSIn i) : SV_TARGET {
    return float4(i.uv, 0.5f, 1.0f);
}
)";

constexpr const char* kBrokenHlsl = R"(
float4 PSMain() : SV_TARGET {
    return undefinedSymbol;  // Forces DXC to error out.
}
)";

constexpr const char* kManifestTemplate = R"({
  "stableId": "user.test_passthrough",
  "displayName": "Test Passthrough",
  "category": "User",
  "shader": "passthrough.hlsl",
  "entryPoint": "PSMain",
  "passCount": 1,
  "params": [
    {"name": "amount", "displayName": "Amount", "type": "Float", "default": 0.5, "min": 0.0, "max": 1.0}
  ]
})";

fs::path makeTempEffectsDir(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
        (std::string("entity_user_effects_") + tag + "_" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

struct TempEffectsDir {
    fs::path path;
    explicit TempEffectsDir(const char* tag) : path(makeTempEffectsDir(tag)) {}
    ~TempEffectsDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const char* content) {
    std::ofstream f(p);
    f << content;
}

} // namespace

TEST(EffectKindRegistry, ScanRegistersUserKindFromManifest) {
    TempEffectsDir tmp("ok");
    writeFile(tmp.path / "passthrough.hlsl",  kPassthroughHlsl);
    writeFile(tmp.path / "passthrough.json", kManifestTemplate);

    entity::effects::EffectKindRegistry reg;
    entity::RuntimeShaderCompiler compiler;
    reg.scanUserEffects(tmp.path, compiler);

    const auto hash = entity::effects::fnv1a32("user.test_passthrough");
    const auto* kind = reg.find(hash);
    ASSERT_NE(kind, nullptr) << "user kind not registered";
    EXPECT_EQ(kind->stableId,    "user.test_passthrough");
    EXPECT_EQ(kind->displayName, "Test Passthrough");
    EXPECT_EQ(kind->category,    "User");
    EXPECT_FALSE(kind->builtin);
    ASSERT_EQ(kind->params.size(), 1u);
    EXPECT_EQ(kind->params[0].name,        "amount");
    EXPECT_EQ(kind->params[0].displayName, "Amount");
    EXPECT_EQ(kind->params[0].type, entity::ParamValue::Type::Float);
    EXPECT_FLOAT_EQ(kind->params[0].defaultValue.f4[0], 0.5f);

    auto bytecode = reg.tryGetUserPsBytecode(hash);
    EXPECT_TRUE(bytecode.valid()) << "user kind compiled but bytecode missing";
    EXPECT_GT(bytecode.size, 0u);
}

TEST(EffectKindRegistry, ScanSkipsBrokenShaderButContinuesOthers) {
    // Two manifests in the same dir — one good, one broken. The good one
    // should still register; the broken one should be skipped.
    TempEffectsDir tmp("mixed");
    writeFile(tmp.path / "passthrough.hlsl",  kPassthroughHlsl);
    writeFile(tmp.path / "passthrough.json", kManifestTemplate);

    writeFile(tmp.path / "broken.hlsl", kBrokenHlsl);
    writeFile(tmp.path / "broken.json", R"({
  "stableId": "user.test_broken",
  "displayName": "Broken",
  "category": "User",
  "shader": "broken.hlsl",
  "entryPoint": "PSMain",
  "params": []
})");

    entity::effects::EffectKindRegistry reg;
    entity::RuntimeShaderCompiler compiler;
    reg.scanUserEffects(tmp.path, compiler);

    EXPECT_NE(reg.find(entity::effects::fnv1a32("user.test_passthrough")), nullptr);
    EXPECT_EQ(reg.find(entity::effects::fnv1a32("user.test_broken")),      nullptr);
}

TEST(EffectKindRegistry, ScanIsIdempotentAndReplacesUserKinds) {
    // First scan registers kind A. We then remove A's manifest, add kind B,
    // and re-scan — the registry should now hold B but not A.
    TempEffectsDir tmp("idempotent");
    writeFile(tmp.path / "passthrough.hlsl",  kPassthroughHlsl);
    writeFile(tmp.path / "passthrough.json", kManifestTemplate);

    entity::effects::EffectKindRegistry reg;
    entity::RuntimeShaderCompiler compiler;
    reg.scanUserEffects(tmp.path, compiler);
    ASSERT_NE(reg.find(entity::effects::fnv1a32("user.test_passthrough")), nullptr);

    // Swap A out, B in. Different stableId, same HLSL source for brevity.
    std::error_code ec;
    fs::remove(tmp.path / "passthrough.json", ec);
    writeFile(tmp.path / "passthrough2.json", R"({
  "stableId": "user.test_passthrough_v2",
  "displayName": "Test Passthrough v2",
  "category": "User",
  "shader": "passthrough.hlsl",
  "entryPoint": "PSMain",
  "params": []
})");

    reg.scanUserEffects(tmp.path, compiler);
    EXPECT_EQ(reg.find(entity::effects::fnv1a32("user.test_passthrough")),     nullptr)
        << "old kind not evicted by second scan";
    EXPECT_NE(reg.find(entity::effects::fnv1a32("user.test_passthrough_v2")),  nullptr)
        << "new kind not registered by second scan";
}

TEST(EffectKindRegistry, ScanPreservesBuiltinsAcrossUserScan) {
    // registerBuiltins followed by scanUserEffects — builtins must survive.
    entity::effects::EffectKindRegistry reg;
    reg.registerBuiltins();
    const std::size_t builtinCount = reg.kinds().size();
    ASSERT_GT(builtinCount, 0u);

    TempEffectsDir tmp("preserve_builtins");
    writeFile(tmp.path / "passthrough.hlsl",  kPassthroughHlsl);
    writeFile(tmp.path / "passthrough.json", kManifestTemplate);

    entity::RuntimeShaderCompiler compiler;
    reg.scanUserEffects(tmp.path, compiler);

    // All builtins still present. Each builtin's stableId starts with "core."
    std::size_t survivingBuiltins = 0;
    for (const auto& [hash, k] : reg.kinds()) {
        if (k.builtin) ++survivingBuiltins;
    }
    EXPECT_EQ(survivingBuiltins, builtinCount);
    EXPECT_NE(reg.find(entity::effects::fnv1a32("user.test_passthrough")), nullptr);
}
