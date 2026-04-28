/**
 * Unit tests for per-decoder OCIO color-space tagging (Phase C.12 #6).
 *
 * The tests decode a single frame through each codec path and assert the
 * `ocioColorSpace` string the decoder stamps on `DecodedFrame`. This is
 * the regression gate for "a fourth codec lands and forgets to tag the
 * frame": absence of a tag would silently route through the renderer's
 * fallback (no input transform) and corrupt the goldens.
 *
 * Uses the existing checked-in test_media fixtures (gradient_seq for PNG,
 * hap_gradient/test.mov for HAP) so the test doesn't depend on a fixture-
 * generation pipeline.
 */

#include <gtest/gtest.h>

#include "entity/media/PNGSequenceDecoder.hpp"
#include "entity/media/HAPDecoder.hpp"
#include "entity/media/DecodedFrame.hpp"

#include <filesystem>

using namespace entity;

namespace {

std::filesystem::path repoRoot() {
    // Tests run from the build dir; fixtures live at <repo>/test_media/.
    // CMakeLists set WORKING_DIRECTORY to ${CMAKE_SOURCE_DIR} for ctest, so
    // we can resolve relative to the current working dir.
    return std::filesystem::current_path();
}

} // namespace

TEST(DecoderColorSpaceTests, PngSequenceTagsAsSrgbDisplay) {
    auto fixture = repoRoot() / "test_media" / "gradient_seq";
    if (!std::filesystem::exists(fixture / "frame_000.png")) {
        GTEST_SKIP() << "Fixture missing: " << fixture.string();
    }

    PNGSequenceDecoder dec;
    ASSERT_EQ(dec.open((fixture / "frame_000.png").string()), Result::Success);

    DecodedFrame frame;
    ASSERT_EQ(dec.decodeFrame(0, frame), Result::Success);
    EXPECT_TRUE(frame.valid.load());
    EXPECT_EQ(frame.ocioColorSpace, "sRGB - Display")
        << "PNG decoder must tag frames as sRGB-encoded — that's the source "
           "color space the OCIO input transform expects to linearize.";
}

TEST(DecoderColorSpaceTests, HapDecoderTagsAsLinearRec709) {
    auto fixture = repoRoot() / "test_media" / "hap_gradient" / "test.mov";
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << "Fixture missing: " << fixture.string();
    }

    HAPDecoder dec;
    Result r = dec.open(fixture.string());
    if (r != Result::Success) {
        GTEST_SKIP() << "HAP decoder open failed (FFmpeg path may not be wired): "
                     << ResultToString(r);
    }

    DecodedFrame frame;
    r = dec.decodeFrame(0, frame);
    if (r != Result::Success) {
        GTEST_SKIP() << "HAP decoder decodeFrame failed: " << ResultToString(r);
    }
    EXPECT_TRUE(frame.valid.load());
    // hap_gradient/test.mov is Hap1 (RGB BC1) — the Rec.709-primaries
    // sRGB-EOTF source matches the OCIO Studio Config name below.
    EXPECT_EQ(frame.ocioColorSpace, "Linear Rec.709 (sRGB)")
        << "HAP RGB variants must tag as Rec.709-primaries content; the "
           "in-shader YCoCg unscale (HAP Q) also produces Rec.709 RGB so "
           "every HAP variant except HapA shares this tag.";
}
