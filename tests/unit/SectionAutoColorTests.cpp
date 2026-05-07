// Round-3 Phase 2 — section auto-color palette unit tests.
//
// Validates `entity::sectionPalette::pickColor(index)`:
// - palette has exactly 8 entries
// - implicit first segment maps to palette[0]
// - mapping is index-modulo so 8+ sections wrap cleanly
// - the 0th palette entry matches `addSectionBreak`'s default color, so
//   command-line / scripted creation that omits the color argument lands
//   on the same hue the UI uses for the implicit first segment.

#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"

using namespace entity;

TEST(SectionAutoColorTest, PaletteSize_IsExactlyEight) {
    EXPECT_EQ(sectionPalette::kSectionColors.size(), 8u);
}

TEST(SectionAutoColorTest, PickColor_IndexZero_IsCoolBlue) {
    EXPECT_EQ(sectionPalette::pickColor(0), 0xFF6090C8u);
}

TEST(SectionAutoColorTest, PickColor_MatchesAddSectionBreakDefault) {
    // `Timeline::addSectionBreak` defaults to 0xFF6090C8u (cool blue).
    // palette[0] must agree so projects authored with the default and
    // projects authored via the auto-color UI line up visually.
    entt::registry registry;
    Timeline timeline(registry);
    EXPECT_TRUE(timeline.addSectionBreak(1000));
    const auto& secs = timeline.getSections();
    ASSERT_EQ(secs.size(), 1u);
    EXPECT_EQ(secs[0].color, sectionPalette::pickColor(0));
}

TEST(SectionAutoColorTest, PickColor_DistinctEntriesAcrossPalette) {
    // The eight entries must all differ; otherwise the auto-color UX
    // would silently collapse to fewer hues at low section counts.
    for (std::size_t i = 0; i < sectionPalette::kSectionColors.size(); ++i) {
        for (std::size_t j = i + 1; j < sectionPalette::kSectionColors.size(); ++j) {
            EXPECT_NE(sectionPalette::pickColor(i), sectionPalette::pickColor(j))
                << "palette[" << i << "] == palette[" << j << "]";
        }
    }
}

TEST(SectionAutoColorTest, PickColor_WrapsModulo) {
    // Indices >= 8 must reuse the palette in cyclic order.
    EXPECT_EQ(sectionPalette::pickColor(8),  sectionPalette::pickColor(0));
    EXPECT_EQ(sectionPalette::pickColor(9),  sectionPalette::pickColor(1));
    EXPECT_EQ(sectionPalette::pickColor(15), sectionPalette::pickColor(7));
    EXPECT_EQ(sectionPalette::pickColor(16), sectionPalette::pickColor(0));
    EXPECT_EQ(sectionPalette::pickColor(99), sectionPalette::pickColor(99 % 8));
}

TEST(SectionAutoColorTest, PickColor_LargeIndex_NoOverflow) {
    // Sanity: pickColor uses size_t so very large indices wrap without UB.
    constexpr std::size_t kHuge = static_cast<std::size_t>(1) << 32;
    EXPECT_EQ(sectionPalette::pickColor(kHuge),
              sectionPalette::pickColor(kHuge % 8));
}
