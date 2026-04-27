/**
 * Unit tests for OcioManager's GPU processor cache + GpuShaderDesc bake.
 *
 * Asserts shader emission contracts WITHOUT touching the GPU — the OCIO
 * GpuShaderDesc API is pure CPU codegen, so these run anywhere the OCIO
 * library + ACES Studio Config builtins are available (i.e. anywhere the
 * existing OcioManagerTests run).
 *
 * Subtask 5 turns these processors into actual D3D12 PSOs; that's where
 * real rendering coverage lives.
 */

#include <gtest/gtest.h>

#include "entity/color/OcioManager.hpp"
#include "entity/color/OcioGpuProcessor.hpp"

#include <algorithm>

using namespace entity;

namespace {

bool containsToken(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST(OcioGpuProcessorTests, BuildsInputProcessorForSrgbToAcescg) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    // Try a few candidate sRGB names — the ACES Studio Config across
    // OCIO 2.4-2.5 has used different aliases ("sRGB - Display", "sRGB -
    // Texture", "sRGB", "Utility - sRGB - Texture"). Iterate the available
    // color spaces to find one that contains "sRGB".
    auto spaces = mgr.listColorSpaces();
    std::string srgbName;
    for (const auto& s : spaces) {
        if (s.find("sRGB") != std::string::npos &&
            s.find("Display") != std::string::npos) {
            srgbName = s;
            break;
        }
    }
    if (srgbName.empty()) {
        for (const auto& s : spaces) {
            if (s.find("sRGB") != std::string::npos) {
                srgbName = s;
                break;
            }
        }
    }
    ASSERT_FALSE(srgbName.empty()) << "No sRGB-flavored color space in builtin config";

    auto proc = mgr.buildInputProcessor(srgbName, "ACEScg");
    ASSERT_NE(proc, nullptr);
    EXPECT_FALSE(proc->shaderText().empty());
    EXPECT_FALSE(proc->functionName().empty());
    EXPECT_TRUE(containsToken(proc->shaderText(), proc->functionName()))
        << "Generated HLSL should define the chosen function name";
    EXPECT_EQ(mgr.inputCacheSize(), 1u);
}

TEST(OcioGpuProcessorTests, InputProcessorCacheReturnsSameInstance) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());
    auto spaces = mgr.listColorSpaces();
    ASSERT_FALSE(spaces.empty());

    auto a = mgr.buildInputProcessor(spaces.front(), "ACEScg");
    auto b = mgr.buildInputProcessor(spaces.front(), "ACEScg");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a.get(), b.get()) << "Cache should return the same shared_ptr instance";
    EXPECT_EQ(mgr.inputCacheSize(), 1u);
}

TEST(OcioGpuProcessorTests, BuildsDisplayProcessorForDefaultDisplayView) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    ASSERT_FALSE(display.empty());
    const std::string view = mgr.getDefaultView(display);
    ASSERT_FALSE(view.empty());

    auto proc = mgr.buildDisplayProcessor("ACEScg", display, view);
    ASSERT_NE(proc, nullptr);
    EXPECT_FALSE(proc->shaderText().empty());
    EXPECT_FALSE(proc->functionName().empty());
    EXPECT_TRUE(containsToken(proc->shaderText(), proc->functionName()))
        << "Generated HLSL should define the chosen function name";
    EXPECT_EQ(mgr.displayCacheSize(), 1u);
    // LUT count is intentionally not asserted: a config's default view can be
    // a pure-matrix transform (no LUTs) — that's fine. The non-empty shader
    // body + matching function name is the real contract.
}

TEST(OcioGpuProcessorTests, AcesSdrViewProducesLutBindings) {
    // The actual ACES tone-mapped SDR pipeline (RRT + ODT) ships LUTs in
    // OCIO's emission. We probe the available views looking for one whose
    // name suggests a real ACES tone curve, then assert that processor
    // pulls in at least one LUT. Catches regressions where the texture-
    // copy path in OcioManager::bakeProcessor stops collecting LUTs.
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const auto displays = mgr.listDisplays();
    ASSERT_FALSE(displays.empty());

    bool found = false;
    for (const auto& d : displays) {
        for (const auto& v : mgr.listViews(d)) {
            // Heuristic: views named like "ACES 1.0 - SDR Video", "Output -
            // sRGB", "ACES SDR" all carry the actual tone-curve LUTs.
            if (v.find("ACES") == std::string::npos &&
                v.find("Output") == std::string::npos) continue;
            auto proc = mgr.buildDisplayProcessor("ACEScg", d, v);
            if (!proc) continue;
            if (proc->textures().size() > 0u) {
                found = true;
                EXPECT_FALSE(proc->shaderText().empty());
                EXPECT_TRUE(containsToken(proc->shaderText(), proc->functionName()));
                break;
            }
        }
        if (found) break;
    }
    EXPECT_TRUE(found) << "Expected at least one ACES/Output view to emit LUT bindings";
}

TEST(OcioGpuProcessorTests, DisplayCacheKeysDistinguishViews) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    auto views = mgr.listViews(display);
    if (views.size() < 2u) {
        GTEST_SKIP() << "Need >=2 views on default display to exercise key disambiguation";
    }

    auto a = mgr.buildDisplayProcessor("ACEScg", display, views[0]);
    auto b = mgr.buildDisplayProcessor("ACEScg", display, views[1]);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a.get(), b.get());
    EXPECT_EQ(mgr.displayCacheSize(), 2u);
}

TEST(OcioGpuProcessorTests, ClearProcessorCacheDropsEntries) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());
    auto spaces = mgr.listColorSpaces();
    ASSERT_FALSE(spaces.empty());

    auto a = mgr.buildInputProcessor(spaces.front(), "ACEScg");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(mgr.inputCacheSize(), 1u);

    mgr.clearProcessorCache();
    EXPECT_EQ(mgr.inputCacheSize(), 0u);
    EXPECT_EQ(mgr.displayCacheSize(), 0u);

    auto b = mgr.buildInputProcessor(spaces.front(), "ACEScg");
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a.get(), b.get()) << "After clearProcessorCache, a fresh build should produce a new instance";
}

TEST(OcioGpuProcessorTests, MissingColorSpaceReturnsNull) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    auto proc = mgr.buildInputProcessor("DefinitelyNotAColorSpace_xyz123", "ACEScg");
    EXPECT_EQ(proc, nullptr);
    EXPECT_EQ(mgr.inputCacheSize(), 0u);
}

TEST(OcioGpuProcessorTests, FunctionNamesAreSafeHlslIdentifiers) {
    // Color-space names like "Linear Rec.709 (sRGB)" must mangle into
    // valid HLSL identifiers. Anything else would break runtime DXC compile
    // when subtask 5 splices the function name into composite_ps.
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());
    auto spaces = mgr.listColorSpaces();

    int checked = 0;
    for (const auto& src : spaces) {
        if (src.find_first_of(" .()-/") == std::string::npos) continue; // already plain
        auto proc = mgr.buildInputProcessor(src, "ACEScg");
        if (!proc) continue; // skip ones not connectible to ACEScg
        const std::string& fn = proc->functionName();
        EXPECT_FALSE(fn.empty());
        for (char c : fn) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_';
            EXPECT_TRUE(ok) << "Function name has non-identifier char '" << c
                            << "' for src='" << src << "': " << fn;
        }
        if (++checked >= 3) break;
    }
    EXPECT_GE(checked, 1) << "Expected at least one color-space name with special chars to mangle";
}
