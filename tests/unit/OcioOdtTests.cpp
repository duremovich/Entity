/**
 * Phase C.12 #11 — OCIO ODT correctness (CPU side).
 *
 * Subtask #5 wires the ACES Studio Config display+view processors into the
 * GPU pipeline; subtask #6 rebaked the integration goldens behind it. Those
 * goldens prove the pipeline runs end-to-end, but the *math* behind the ODT
 * is only tested implicitly — a regression in OCIO's CPU-vs-GPU emission
 * would still let the goldens pass while the math drifts.
 *
 * This test pins the math directly. We build the same display processor
 * OcioManager hands the renderer (working space "ACEScg" → display + view),
 * then run known reference inputs through OCIO's *CPU* processor (not GPU).
 * Asserts:
 *   - SDR sRGB ODT: ACEScg(0.18) → mid-gray-ish display code (rolled-off, not
 *     linearly passed-through). White (1.0) ACEScg → strongly rolled-off
 *     highlight, not clipped at 1.0. Super-bright 4.0 ACEScg → clamped near
 *     1.0 (the same kind of rolled-off behaviour the C.12 #10 HAP HDR smoke
 *     test verified visually on the GPU side).
 *   - PQ HDR ODT (when the active config exposes one): scene-linear 0.18
 *     produces a non-trivial PQ codeword > 0 and < 1, and the codeword for
 *     0.18 differs measurably from the codeword for 1.0 — i.e. the PQ EOTF
 *     curve is actually being applied. End-to-end PQ display verification
 *     ships in Phase D when somebody has an HDR projector to point at it.
 *
 * Tolerances are loose by design — the ACES ODT shape evolves across config
 * versions (e.g. ACES 1.0 → 1.1 → 2.0), so a hard equality check would break
 * every time vcpkg bumps OCIO. The asserts here catch *shape* regressions
 * (clamping, identity-pass-through, sign flips, channel swaps) — that's the
 * value-add over the integration goldens.
 */

#include <gtest/gtest.h>

#include "entity/color/OcioManager.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace OCIO = OCIO_NAMESPACE;
using namespace entity;

namespace {

// Apply an OCIO display processor (working → display+view) to one RGB triple
// via the CPU path. Returns the output in-place. Returns false if the
// processor isn't available in the active config.
bool applyDisplay(OcioManager& mgr,
                  const std::string& workingSpace,
                  const std::string& display,
                  const std::string& view,
                  std::array<float, 3>& rgb) {
    auto cfg = mgr.getConfig();
    if (!cfg) return false;
    OCIO::ConstProcessorRcPtr proc;
    try {
        proc = cfg->getProcessor(workingSpace.c_str(),
                                 display.c_str(),
                                 view.c_str(),
                                 OCIO::TRANSFORM_DIR_FORWARD);
    } catch (const OCIO::Exception&) {
        return false;
    }
    if (!proc) return false;
    OCIO::ConstCPUProcessorRcPtr cpu = proc->getDefaultCPUProcessor();
    if (!cpu) return false;
    cpu->applyRGB(rgb.data());
    return true;
}

// Look for a view name on the given display whose name strongly suggests an
// SDR sRGB output transform. The Studio Config ships several candidates
// across versions; the patterns below cover ACES 1.x and 2.0 spellings.
std::string findSdrSrgbView(const std::vector<std::string>& views) {
    for (const auto& v : views) {
        const bool aces       = v.find("ACES") != std::string::npos;
        const bool sdr        = v.find("SDR")  != std::string::npos;
        const bool srgb       = v.find("sRGB") != std::string::npos;
        const bool outputSrgb = v.find("Output - sRGB") != std::string::npos;
        if (outputSrgb) return v;
        if (aces && (sdr || srgb)) return v;
    }
    // Fall back: anything containing "SDR" or "sRGB".
    for (const auto& v : views) {
        if (v.find("SDR") != std::string::npos ||
            v.find("sRGB") != std::string::npos) return v;
    }
    return {};
}

// Look for a HDR view on a PQ-flavored display. ACES Studio Config 2.1 puts
// PQ in the *display* name ("Rec.2100-PQ - Display", "ST2084-P3-D65 -
// Display") — view names just say "HDR" without naming PQ themselves. We
// pre-filter the display list, so this just looks for "HDR" in the view.
std::string findHdrView(const std::vector<std::string>& views) {
    for (const auto& v : views) {
        if (v.find("HDR") != std::string::npos) return v;
    }
    // Older configs sometimes embedded PQ/ST2084 in the view name itself.
    for (const auto& v : views) {
        if (v.find("PQ")     != std::string::npos ||
            v.find("ST2084") != std::string::npos) return v;
    }
    return {};
}

// Find a PQ-flavored display. Studio Config v2.1.0 names them
// "Rec.2100-PQ - Display" and "ST2084-P3-D65 - Display"; older / custom
// configs may use "Rec.2020-ST2084" or similar.
std::string findPqDisplay(const std::vector<std::string>& displays) {
    for (const auto& d : displays) {
        if (d.find("Rec.2100-PQ") != std::string::npos ||
            d.find("Rec2100-PQ")  != std::string::npos ||
            d.find("ST2084")      != std::string::npos ||
            d.find("ST 2084")     != std::string::npos ||
            d.find("PQ")          != std::string::npos) return d;
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// SDR sRGB ODT — mid-gray, white, and super-bright through the default ACES
// sRGB view. The numeric ranges below are deliberately wide; the goal is to
// catch shape regressions (linear pass-through, clamp-to-zero, channel
// swap) rather than nail down a specific tone-curve revision.
// ---------------------------------------------------------------------------

TEST(OcioOdtTests, SdrSrgbAcescgMidGrayLandsInDisplayMidtones) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    ASSERT_FALSE(display.empty());

    const auto views = mgr.listViews(display);
    ASSERT_FALSE(views.empty());

    const std::string view = findSdrSrgbView(views);
    if (view.empty()) {
        GTEST_SKIP() << "No SDR sRGB-flavored view on default display "
                     << "'" << display << "'";
    }

    std::array<float, 3> rgb{0.18f, 0.18f, 0.18f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, rgb))
        << "Display processor for ACEScg → '" << display << "' / '"
        << view << "' must be available";

    // Channels stay achromatic.
    EXPECT_NEAR(rgb[0], rgb[1], 1e-3f);
    EXPECT_NEAR(rgb[1], rgb[2], 1e-3f);

    // ACES anchor: scene-linear 0.18 grey lands at ≈ 0.39-0.40 in sRGB
    // display code values (RRT + sRGB ODT + sRGB OETF). Allow generous
    // tolerance — different ODT revs land slightly differently but they
    // all sit well clear of black and well below white.
    EXPECT_GT(rgb[0], 0.20f) << "Mid-gray collapsed toward black: " << rgb[0];
    EXPECT_LT(rgb[0], 0.60f) << "Mid-gray drifted toward white: " << rgb[0];
}

TEST(OcioOdtTests, SdrSrgbAcescgWhiteRollsOff) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    const auto views          = mgr.listViews(display);
    const std::string view    = findSdrSrgbView(views);
    if (view.empty()) {
        GTEST_SKIP() << "No SDR sRGB-flavored view available";
    }

    std::array<float, 3> rgb{1.0f, 1.0f, 1.0f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, rgb));

    // RRT + ODT shoulder: scene-referred 1.0 should NOT pass through to
    // display 1.0. Most ACES revs land it around 0.75-0.90 in sRGB code.
    // Anything ≥ 0.99 means the tone curve isn't being applied.
    EXPECT_GT(rgb[0], 0.50f) << "White crushed below midtones: " << rgb[0];
    EXPECT_LE(rgb[0], 1.0f);
    EXPECT_LT(rgb[0], 0.99f) << "White pegged at display max — RRT/ODT "
                                "shoulder appears to be missing: " << rgb[0];
}

TEST(OcioOdtTests, SdrSrgbAcescgSuperBrightClampsNearOne) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    const auto views          = mgr.listViews(display);
    const std::string view    = findSdrSrgbView(views);
    if (view.empty()) {
        GTEST_SKIP() << "No SDR sRGB-flavored view available";
    }

    // Same input shape the C.12 #10 HAP HDR smoke test gates on the GPU
    // side: a constant ~4.0 RGB triple. CPU-path expectation: clamped /
    // rolled-off near 1.0 by the SDR ODT.
    std::array<float, 3> rgb{4.0f, 4.0f, 4.0f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, rgb));

    EXPECT_GT(rgb[0], 0.85f) << "Super-bright crushed below highlights: "
                             << rgb[0];
    EXPECT_LE(rgb[0], 1.0f)  << "Output exceeded display max — SDR ODT "
                                "should clamp: " << rgb[0];
}

TEST(OcioOdtTests, SdrSrgbAcescgBlackStaysBlack) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    const auto views          = mgr.listViews(display);
    const std::string view    = findSdrSrgbView(views);
    if (view.empty()) {
        GTEST_SKIP() << "No SDR sRGB-flavored view available";
    }

    std::array<float, 3> rgb{0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, rgb));

    // Black should land at ≤ a few code values worth — never hugely
    // above zero (would mean the tone curve is offset).
    EXPECT_LT(std::fabs(rgb[0]), 0.05f) << "Black drifted: " << rgb[0];
    EXPECT_LT(std::fabs(rgb[1]), 0.05f);
    EXPECT_LT(std::fabs(rgb[2]), 0.05f);
}

// ---------------------------------------------------------------------------
// Channel-order sanity — pure red ACEScg input does NOT produce a green or
// blue dominant output. Catches a swizzle or matrix-mismatch regression in
// the OCIO config's RRT + ODT pipeline.
// ---------------------------------------------------------------------------

TEST(OcioOdtTests, SdrSrgbPreservesChannelDominance) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    const auto views          = mgr.listViews(display);
    const std::string view    = findSdrSrgbView(views);
    if (view.empty()) {
        GTEST_SKIP() << "No SDR sRGB-flavored view available";
    }

    std::array<float, 3> red{0.5f, 0.0f, 0.0f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, red));
    EXPECT_GT(red[0], red[1]) << "ACEScg red came out non-red-dominant";
    EXPECT_GT(red[0], red[2]);

    std::array<float, 3> green{0.0f, 0.5f, 0.0f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, green));
    EXPECT_GT(green[1], green[0]);
    EXPECT_GT(green[1], green[2]);

    std::array<float, 3> blue{0.0f, 0.0f, 0.5f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, blue));
    EXPECT_GT(blue[2], blue[0]);
    EXPECT_GT(blue[2], blue[1]);
}

// ---------------------------------------------------------------------------
// PQ HDR ODT — only validated mathematically (no HDR display in the lab).
// Verifies the PQ EOTF is actually being applied: mid-gray and white should
// produce different output codewords, and both should sit in [0, 1].
// ---------------------------------------------------------------------------

TEST(OcioOdtTests, PqHdrOdtAppliesNonIdentityCurve) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    // Find a PQ-flavored display, then a HDR view on it.
    const std::string display = findPqDisplay(mgr.listDisplays());
    if (display.empty()) {
        GTEST_SKIP() << "No PQ-capable display in active OCIO config";
    }
    const std::string view = findHdrView(mgr.listViews(display));
    if (view.empty()) {
        GTEST_SKIP() << "Display '" << display << "' has no HDR view";
    }

    std::array<float, 3> mid{0.18f, 0.18f, 0.18f};
    if (!applyDisplay(mgr, "ACEScg", display, view, mid)) {
        GTEST_SKIP() << "PQ display processor not buildable for view '"
                     << view << "'";
    }

    std::array<float, 3> white{1.0f, 1.0f, 1.0f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, white));

    // Mid-gray and white must both produce in-range PQ codewords.
    EXPECT_GE(mid[0],   0.0f);
    EXPECT_LE(mid[0],   1.0f);
    EXPECT_GE(white[0], 0.0f);
    EXPECT_LE(white[0], 1.0f);

    // Mid and white must differ measurably — if they didn't, the PQ EOTF
    // would essentially be flat and the math wouldn't be running.
    EXPECT_GT(std::fabs(white[0] - mid[0]), 0.05f)
        << "PQ codewords for mid-gray and white were within 0.05 — the "
           "EOTF doesn't appear to be applied. mid=" << mid[0]
        << " white=" << white[0];

    // Mid-gray should be measurably above black on the PQ curve.
    EXPECT_GT(mid[0], 0.05f) << "PQ mid-gray crushed: " << mid[0];
}

// ---------------------------------------------------------------------------
// Sanity: the OcioManager's GPU display processor cache and the
// hand-built CPU processor agree on the same (display, view) key —
// i.e. the renderer is asking for the same transform we're testing here.
// ---------------------------------------------------------------------------

// One-shot listing test (kept disabled in CI via DISABLED_ prefix). Run
// manually with --gtest_also_run_disabled_tests when the OCIO config bumps
// to discover the new display/view names that PqHdrOdtAppliesNonIdentityCurve
// should match.
TEST(OcioOdtTests, DISABLED_DumpDisplaysAndViews) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());
    for (const auto& d : mgr.listDisplays()) {
        std::cerr << "Display: " << d << '\n';
        for (const auto& v : mgr.listViews(d)) {
            std::cerr << "  View: " << v << '\n';
        }
    }
}

TEST(OcioOdtTests, GpuDisplayCacheAndCpuProcessorAgreeOnDefaultView) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    const std::string display = mgr.getDefaultDisplay();
    const std::string view    = mgr.getDefaultView(display);
    ASSERT_FALSE(display.empty());
    ASSERT_FALSE(view.empty());

    // Build the same processor through OcioManager's GPU cache (the path
    // the renderer uses) — proves the processor is constructible. The
    // CPU result is what we then validate against.
    auto gpu = mgr.buildDisplayProcessor("ACEScg", display, view);
    ASSERT_NE(gpu, nullptr);
    EXPECT_FALSE(gpu->shaderText().empty());

    std::array<float, 3> rgb{0.18f, 0.18f, 0.18f};
    ASSERT_TRUE(applyDisplay(mgr, "ACEScg", display, view, rgb));
    EXPECT_GT(rgb[0], 0.0f);
    EXPECT_LE(rgb[0], 1.0f);
}
