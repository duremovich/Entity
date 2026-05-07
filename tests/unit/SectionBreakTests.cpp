// Phase B — unit tests for the section break-point Timeline API. The data
// model (after the Phase B refactor):
// - Vector kept sorted ascending by `breakFrame`.
// - Insert via lower_bound, dup-rejected (exact-frame collisions).
// - Lookup is binary search (findNextBreakAfter is the SectionScheduler hot
//   path; findSectionBreakNear backs the ruler hit-test).
// - Edit can change `breakFrame`, which re-sorts; collisions rejected.

#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {

class SectionBreakTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
    }
};

} // namespace

TEST_F(SectionBreakTest, SortedInsert_OutOfOrderInputProducesSortedVector) {
    EXPECT_TRUE(timeline->addSectionBreak(2000));
    EXPECT_TRUE(timeline->addSectionBreak(1000));
    EXPECT_TRUE(timeline->addSectionBreak(3000));
    EXPECT_TRUE(timeline->addSectionBreak(1500));

    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 4u);
    EXPECT_EQ(secs[0].breakFrame, 1000);
    EXPECT_EQ(secs[1].breakFrame, 1500);
    EXPECT_EQ(secs[2].breakFrame, 2000);
    EXPECT_EQ(secs[3].breakFrame, 3000);
}

TEST_F(SectionBreakTest, AddDuplicateFrame_Rejected) {
    EXPECT_TRUE(timeline->addSectionBreak(1000, 0xAABBCCDDu));
    EXPECT_FALSE(timeline->addSectionBreak(1000, 0x11223344u));

    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 1u);
    EXPECT_EQ(secs[0].color, 0xAABBCCDDu);
}

TEST_F(SectionBreakTest, RemoveSectionBreak_RemovesAndPreservesOrder) {
    timeline->addSectionBreak(1000);
    timeline->addSectionBreak(2000);
    timeline->addSectionBreak(3000);

    EXPECT_TRUE(timeline->removeSectionBreak(2000));
    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 2u);
    EXPECT_EQ(secs[0].breakFrame, 1000);
    EXPECT_EQ(secs[1].breakFrame, 3000);

    EXPECT_FALSE(timeline->removeSectionBreak(99999));
}

TEST_F(SectionBreakTest, FindNextBreakAfter_BinarySearchEdges) {
    timeline->addSectionBreak(1000);
    timeline->addSectionBreak(2000);
    timeline->addSectionBreak(3000);

    // Strictly after — first break.
    auto* a = timeline->findNextBreakAfter(0);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->breakFrame, 1000);

    // Sitting exactly on a break — next is the one after it.
    auto* b = timeline->findNextBreakAfter(1000);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->breakFrame, 2000);

    // Between two — yields the next.
    auto* c = timeline->findNextBreakAfter(2500);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->breakFrame, 3000);

    // Past the last — nullptr.
    EXPECT_EQ(timeline->findNextBreakAfter(5000), nullptr);
    EXPECT_EQ(timeline->findNextBreakAfter(3000), nullptr);
}

TEST_F(SectionBreakTest, FindSectionBreakNear_RespectsTolerance) {
    timeline->addSectionBreak(1000);
    timeline->addSectionBreak(5000);

    auto* exact = timeline->findSectionBreakNear(1000, 0);
    ASSERT_NE(exact, nullptr);
    EXPECT_EQ(exact->breakFrame, 1000);

    auto* withinTol = timeline->findSectionBreakNear(1003, 5);
    ASSERT_NE(withinTol, nullptr);
    EXPECT_EQ(withinTol->breakFrame, 1000);

    EXPECT_EQ(timeline->findSectionBreakNear(2500, 100), nullptr);

    auto* belowExact = timeline->findSectionBreakNear(998, 5);
    ASSERT_NE(belowExact, nullptr);
    EXPECT_EQ(belowExact->breakFrame, 1000);
}

TEST_F(SectionBreakTest, EditSectionBreak_ChangeFields_FrameUnchanged) {
    timeline->addSectionBreak(1000, 0xAABBCCDDu, 0.0);
    EXPECT_TRUE(timeline->editSectionBreak(1000, 1000, 0x11223344u, 0.5));

    auto* live = timeline->findSectionBreakNear(1000, 0);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(live->color, 0x11223344u);
    EXPECT_DOUBLE_EQ(live->fadeSeconds, 0.5);
}

TEST_F(SectionBreakTest, EditSectionBreak_ChangeFrame_ResortsVector) {
    timeline->addSectionBreak(1000);
    timeline->addSectionBreak(2000);
    timeline->addSectionBreak(3000);

    // Move the middle break beyond the last.
    EXPECT_TRUE(timeline->editSectionBreak(2000, 5000, 0u, 0.0));

    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 3u);
    EXPECT_EQ(secs[0].breakFrame, 1000);
    EXPECT_EQ(secs[1].breakFrame, 3000);
    EXPECT_EQ(secs[2].breakFrame, 5000);
}

TEST_F(SectionBreakTest, EditSectionBreak_CollidingFrame_Rejected) {
    timeline->addSectionBreak(1000);
    timeline->addSectionBreak(2000);

    EXPECT_FALSE(timeline->editSectionBreak(1000, 2000, 0u, 0.0));

    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 2u);
    EXPECT_EQ(secs[0].breakFrame, 1000);
    EXPECT_EQ(secs[1].breakFrame, 2000);
}

TEST_F(SectionBreakTest, ClearOnTimelineClear) {
    timeline->addSectionBreak(1000);
    timeline->addSectionBreak(2000);
    timeline->clear();
    EXPECT_TRUE(timeline->getSections().empty());
}

TEST_F(SectionBreakTest, AtSectionBreakFlag_RoundTrips) {
    EXPECT_FALSE(timeline->sectionAtBreak());
    timeline->setSectionAtBreak(true);
    EXPECT_TRUE(timeline->sectionAtBreak());
    timeline->setSectionAtBreak(false);
    EXPECT_FALSE(timeline->sectionAtBreak());
}

// Round-3 Phase 1 — `Timeline::seek()` clears the at-break latch. Manual
// seeks (UI scrub, script SeekToFrame, command playback) must not leave
// stale at-break state for the Phase 5 visibility gate to misfire on.
// The flag's owner is SectionScheduler's park flow; any other state
// mutation invalidates it. See `docs/adr/0012-timeline-sections-and-cues.md`
// Round-3 amendment for the full state-ownership story.
TEST_F(SectionBreakTest, ManualSeek_ClearsAtSectionBreakFlag) {
    // Simulate the parked-at-break state SectionScheduler would normally
    // produce: scheduler raises the latch on a break crossing, then the
    // user manually scrubs elsewhere.
    timeline->setSectionAtBreak(true);
    ASSERT_TRUE(timeline->sectionAtBreak());

    // Any seek (forward or backward) clears the latch.
    timeline->seek(1234567);
    EXPECT_FALSE(timeline->sectionAtBreak());

    // Re-arming + zero-time seek also clears (degenerate but valid case).
    timeline->setSectionAtBreak(true);
    timeline->seek(0);
    EXPECT_FALSE(timeline->sectionAtBreak());

    // Out-of-range seek (negative time) still clears even though the
    // clamp puts currentTime at 0. The clearing semantics are unconditional.
    timeline->setSectionAtBreak(true);
    timeline->seek(-1);
    EXPECT_FALSE(timeline->sectionAtBreak());
}

TEST_F(SectionBreakTest, ManualSeek_NoBreaksLatched_StillAClear) {
    // Defensive: even when the latch was already false, seek() leaves it
    // false. This catches a regression where someone might "optimize" by
    // skipping the clear when the flag wasn't set.
    EXPECT_FALSE(timeline->sectionAtBreak());
    timeline->seek(500000);
    EXPECT_FALSE(timeline->sectionAtBreak());
}
