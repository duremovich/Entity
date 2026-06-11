// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Unit tests for entity::media::premultiplyRgbaRows — the fused
// alpha-premultiply + row-copy pass extracted from ProResDecoder::convertToRGBA.
//
// Coverage:
//   - Premultiply math correctness for a == 0, a == 255, a == 128 (rounding).
//   - Alpha channel preserved untouched.
//   - Differing src / dst row pitches (cached tight-packed scratch ->
//     256-aligned upload-heap footprint).
//   - dst write-only: structurally guaranteed by the function (every dst byte
//     is assigned, none read). Verified here by pre-poisoning dst and
//     confirming the result depends only on src, not on prior dst contents.

#include "entity/media/AlphaPremultiply.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using entity::media::premultiplyRgbaRows;

namespace {

// Reference implementation of the round-to-nearest div255 the helper uses.
uint8_t premul1(uint8_t c, uint8_t a) {
    uint32_t t = static_cast<uint32_t>(c) * a + 128;
    return static_cast<uint8_t>((t + (t >> 8)) >> 8);
}

} // namespace

// ---------------------------------------------------------------------------
// Math: known values for the edge alphas.
// ---------------------------------------------------------------------------

TEST(AlphaPremultiply, AlphaZeroZeroesColor) {
    // RGBA = (200, 100, 50, 0) -> color premultiplied to 0, alpha stays 0.
    std::vector<uint8_t> src = {200, 100, 50, 0};
    std::vector<uint8_t> dst(4, 0xAB);
    premultiplyRgbaRows(src.data(), 4, dst.data(), 4, 1, 1);
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], 0);
    EXPECT_EQ(dst[3], 0);
}

TEST(AlphaPremultiply, AlphaFullIsIdentity) {
    // a == 255 must be an exact identity on the color channels.
    std::vector<uint8_t> src = {17, 200, 255, 255};
    std::vector<uint8_t> dst(4, 0xAB);
    premultiplyRgbaRows(src.data(), 4, dst.data(), 4, 1, 1);
    EXPECT_EQ(dst[0], 17);
    EXPECT_EQ(dst[1], 200);
    EXPECT_EQ(dst[2], 255);
    EXPECT_EQ(dst[3], 255);
}

TEST(AlphaPremultiply, AlphaHalfRoundsToNearest) {
    // a == 128. Expected via the same round-to-nearest formula.
    std::vector<uint8_t> src = {255, 128, 1, 128};
    std::vector<uint8_t> dst(4, 0xAB);
    premultiplyRgbaRows(src.data(), 4, dst.data(), 4, 1, 1);
    EXPECT_EQ(dst[0], premul1(255, 128));  // 128
    EXPECT_EQ(dst[1], premul1(128, 128));  // 64
    EXPECT_EQ(dst[2], premul1(1, 128));    // 1 (0.5 rounds up)
    EXPECT_EQ(dst[3], 128);
}

// Exhaustive check of the math against the reference for a sweep of values.
TEST(AlphaPremultiply, MatchesReferenceAcrossSweep) {
    std::vector<uint8_t> src;
    std::vector<uint8_t> expected;
    for (int c = 0; c <= 255; c += 5) {
        for (int a = 0; a <= 255; a += 5) {
            src.push_back(static_cast<uint8_t>(c));        // R
            src.push_back(static_cast<uint8_t>(255 - c));  // G
            src.push_back(static_cast<uint8_t>(c / 2));    // B
            src.push_back(static_cast<uint8_t>(a));        // A
            expected.push_back(premul1(static_cast<uint8_t>(c), static_cast<uint8_t>(a)));
            expected.push_back(premul1(static_cast<uint8_t>(255 - c), static_cast<uint8_t>(a)));
            expected.push_back(premul1(static_cast<uint8_t>(c / 2), static_cast<uint8_t>(a)));
            expected.push_back(static_cast<uint8_t>(a));
        }
    }
    const uint32_t pixels = static_cast<uint32_t>(src.size() / 4);
    std::vector<uint8_t> dst(src.size(), 0);
    premultiplyRgbaRows(src.data(), src.size(), dst.data(), dst.size(), pixels, 1);
    EXPECT_EQ(dst, expected);
}

// ---------------------------------------------------------------------------
// Differing src / dst pitches: cached scratch tight-packed, dst padded to a
// 256-aligned footprint. Padding bytes in dst must be left untouched and the
// pixel data must land at the right offsets.
// ---------------------------------------------------------------------------

TEST(AlphaPremultiply, DifferingPitchesCopiesEachRowCorrectly) {
    const uint32_t width = 3;
    const uint32_t height = 2;
    const size_t srcPitch = static_cast<size_t>(width) * 4;  // 12, tight
    const size_t dstPitch = 256;                              // padded

    std::vector<uint8_t> src(srcPitch * height);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    // Force alpha = 255 on every pixel so premultiply is identity and we can
    // compare the copied bytes directly.
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col)
            src[row * srcPitch + col * 4 + 3] = 255;

    const uint8_t kPoison = 0xCD;
    std::vector<uint8_t> dst(dstPitch * height, kPoison);
    premultiplyRgbaRows(src.data(), srcPitch, dst.data(), dstPitch, width, height);

    for (uint32_t row = 0; row < height; ++row) {
        // Pixel region matches src (identity premultiply).
        for (uint32_t b = 0; b < width * 4; ++b) {
            EXPECT_EQ(dst[row * dstPitch + b], src[row * srcPitch + b])
                << "row " << row << " byte " << b;
        }
        // Padding after the pixel data is untouched.
        for (size_t b = width * 4; b < dstPitch; ++b) {
            EXPECT_EQ(dst[row * dstPitch + b], kPoison)
                << "padding row " << row << " byte " << b;
        }
    }
}

// ---------------------------------------------------------------------------
// dst is write-only: result must depend only on src, regardless of what dst
// held before. Run twice with two different poison fills and confirm identical
// output. (The function cannot read dst and still be deterministic here.)
// ---------------------------------------------------------------------------

TEST(AlphaPremultiply, DestinationIsWriteOnly) {
    const uint32_t width = 8;
    const uint32_t height = 4;
    const size_t pitch = static_cast<size_t>(width) * 4;

    std::vector<uint8_t> src(pitch * height);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>((i * 13 + 5) & 0xFF);

    std::vector<uint8_t> dstA(pitch * height, 0x00);
    std::vector<uint8_t> dstB(pitch * height, 0xFF);
    premultiplyRgbaRows(src.data(), pitch, dstA.data(), pitch, width, height);
    premultiplyRgbaRows(src.data(), pitch, dstB.data(), pitch, width, height);

    EXPECT_EQ(dstA, dstB);
}
