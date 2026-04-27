/**
 * Unit tests for RuntimeShaderCompiler.
 *
 * Validates the in-memory DXC path that subtask 5 will use to splice OCIO-
 * generated function source onto our existing pixel shaders. Compiles tiny
 * passthrough shaders from string literals so the tests don't depend on
 * any on-disk shader files.
 */

#include <gtest/gtest.h>

#include "entity/render/RuntimeShaderCompiler.hpp"

using namespace entity;

namespace {

constexpr const char* kPassthroughPS = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.uv, 0.5, 1.0);
}
)";

constexpr const char* kBrokenPS = R"(
float4 PSMain() : SV_TARGET {
    // Reference an undefined identifier so DXC errors out.
    return undefinedSymbol;
}
)";

} // namespace

TEST(RuntimeShaderCompilerTests, CompilesPassthroughPixelShader) {
    RuntimeShaderCompiler c;
    auto result = c.compile(kPassthroughPS, "PSMain", "ps_6_0");
    EXPECT_TRUE(result.success) << "Compile diag: " << result.errors;
    ASSERT_NE(result.blob, nullptr);
    EXPECT_GT(result.blob->GetBufferSize(), 0u);

    auto bc = result.asBytecode();
    EXPECT_EQ(bc.pShaderBytecode, result.blob->GetBufferPointer());
    EXPECT_EQ(bc.BytecodeLength, result.blob->GetBufferSize());
}

TEST(RuntimeShaderCompilerTests, ReportsErrorOnBrokenShader) {
    RuntimeShaderCompiler c;
    auto result = c.compile(kBrokenPS, "PSMain", "ps_6_0");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.blob, nullptr);
    EXPECT_FALSE(result.errors.empty()) << "Expected DXC diag in errors";
    // Sanity-check the diag actually mentions the bad identifier.
    EXPECT_NE(result.errors.find("undefinedSymbol"), std::string::npos)
        << "Errors did not include the failing identifier:\n" << result.errors;
}

TEST(RuntimeShaderCompilerTests, CompileAndCacheReturnsCachedBlob) {
    RuntimeShaderCompiler c;
    auto first  = c.compileAndCache(kPassthroughPS, "PSMain", "ps_6_0");
    ASSERT_TRUE(first.success);
    ASSERT_NE(first.blob, nullptr);
    EXPECT_EQ(c.cacheSize(), 1u);

    auto second = c.compileAndCache(kPassthroughPS, "PSMain", "ps_6_0");
    ASSERT_TRUE(second.success);
    EXPECT_EQ(second.blob.Get(), first.blob.Get())
        << "Second compile should return the cached IDxcBlob";
    EXPECT_EQ(c.cacheSize(), 1u);
}

TEST(RuntimeShaderCompilerTests, CacheKeysDistinguishEntryPoint) {
    constexpr const char* twoEntries = R"(
        float4 EntryA() : SV_TARGET { return float4(1, 0, 0, 1); }
        float4 EntryB() : SV_TARGET { return float4(0, 1, 0, 1); }
    )";
    RuntimeShaderCompiler c;
    auto a = c.compileAndCache(twoEntries, "EntryA", "ps_6_0");
    auto b = c.compileAndCache(twoEntries, "EntryB", "ps_6_0");
    ASSERT_TRUE(a.success);
    ASSERT_TRUE(b.success);
    EXPECT_NE(a.blob.Get(), b.blob.Get());
    EXPECT_EQ(c.cacheSize(), 2u);
}

TEST(RuntimeShaderCompilerTests, ClearCacheDropsEntries) {
    RuntimeShaderCompiler c;
    auto r = c.compileAndCache(kPassthroughPS, "PSMain", "ps_6_0");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(c.cacheSize(), 1u);
    c.clearCache();
    EXPECT_EQ(c.cacheSize(), 0u);
}

TEST(RuntimeShaderCompilerTests, CachesFailedCompiles) {
    // We don't want a transient compile failure to keep retrying every frame
    // when an OCIO config produces a bad shader. The cache should remember
    // the failure too — and a clearCache() forces a re-attempt.
    RuntimeShaderCompiler c;
    auto fail1 = c.compileAndCache(kBrokenPS, "PSMain", "ps_6_0");
    EXPECT_FALSE(fail1.success);
    EXPECT_EQ(c.cacheSize(), 1u);

    auto fail2 = c.compileAndCache(kBrokenPS, "PSMain", "ps_6_0");
    EXPECT_FALSE(fail2.success);
    EXPECT_EQ(fail2.errors, fail1.errors);
    EXPECT_EQ(c.cacheSize(), 1u);
}
