// Phase A — unit tests for Timeline cue tag API. The data model:
// - Vector kept sorted ascending by `number`.
// - Insert via lower_bound, dup-rejected.
// - Lookup is binary search.
// - Edit can change `number`, which re-sorts; collisions rejected.

#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/timeline/CueTag.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {

class CueTagTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
    }

    void add(double number, FrameNumber frame, std::string label = {}) {
        CueTag c;
        c.number = number;
        c.frame = frame;
        c.label = std::move(label);
        ASSERT_TRUE(timeline->addCueTag(std::move(c)));
    }
};

} // namespace

TEST_F(CueTagTest, AddSorted_OutOfOrderInputProducesSortedVector) {
    add(2.0, 4000000);
    add(1.0, 1000000);
    add(1.5, 2000000);
    add(100.25, 9000000);

    const auto& cues = timeline->getCueTags();
    ASSERT_EQ(cues.size(), 4u);
    EXPECT_DOUBLE_EQ(cues[0].number, 1.0);
    EXPECT_DOUBLE_EQ(cues[1].number, 1.5);
    EXPECT_DOUBLE_EQ(cues[2].number, 2.0);
    EXPECT_DOUBLE_EQ(cues[3].number, 100.25);
}

TEST_F(CueTagTest, AddDuplicateNumber_Rejected) {
    add(1.0, 1000000);
    CueTag dup;
    dup.number = 1.0;
    dup.frame = 9999999;
    EXPECT_FALSE(timeline->addCueTag(dup));

    // Original entry untouched.
    const CueTag* live = timeline->findCueTag(1.0);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(live->frame, 1000000);
}

TEST_F(CueTagTest, FindCueTag_BinarySearchHitsAndMisses) {
    add(1.0, 100);
    add(2.5, 250);
    add(7.5, 750);

    EXPECT_NE(timeline->findCueTag(1.0), nullptr);
    EXPECT_NE(timeline->findCueTag(2.5), nullptr);
    EXPECT_NE(timeline->findCueTag(7.5), nullptr);

    EXPECT_EQ(timeline->findCueTag(0.5), nullptr);
    EXPECT_EQ(timeline->findCueTag(2.0), nullptr);   // between existing
    EXPECT_EQ(timeline->findCueTag(8.0), nullptr);   // past end
    EXPECT_EQ(timeline->findCueTag(2.5001), nullptr); // close but not equal
}

TEST_F(CueTagTest, RemoveCueTag_RemovesAndPreservesOrder) {
    add(1.0, 1);
    add(2.0, 2);
    add(3.0, 3);

    EXPECT_TRUE(timeline->removeCueTag(2.0));
    const auto& cues = timeline->getCueTags();
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_DOUBLE_EQ(cues[0].number, 1.0);
    EXPECT_DOUBLE_EQ(cues[1].number, 3.0);

    EXPECT_FALSE(timeline->removeCueTag(99.0));  // missing key
}

TEST_F(CueTagTest, EditCueTag_ChangeFields_NumberUnchanged) {
    add(1.0, 100, "old");
    EXPECT_TRUE(timeline->editCueTag(1.0, 1.0, 5000, "new"));

    const CueTag* live = timeline->findCueTag(1.0);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(live->frame, 5000);
    EXPECT_EQ(live->label, "new");
}

TEST_F(CueTagTest, EditCueTag_ChangeNumber_ResortsVector) {
    add(1.0, 100);
    add(2.0, 200);
    add(3.0, 300);

    // Move the middle cue to a number larger than the last entry.
    EXPECT_TRUE(timeline->editCueTag(2.0, 5.0, 200, ""));

    const auto& cues = timeline->getCueTags();
    ASSERT_EQ(cues.size(), 3u);
    EXPECT_DOUBLE_EQ(cues[0].number, 1.0);
    EXPECT_DOUBLE_EQ(cues[1].number, 3.0);
    EXPECT_DOUBLE_EQ(cues[2].number, 5.0);
}

TEST_F(CueTagTest, EditCueTag_CollidingNumber_Rejected) {
    add(1.0, 100);
    add(2.0, 200);

    // Trying to rename 1.0 -> 2.0 should fail (2.0 already exists).
    EXPECT_FALSE(timeline->editCueTag(1.0, 2.0, 100, ""));

    // Both originals still present.
    const auto& cues = timeline->getCueTags();
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_DOUBLE_EQ(cues[0].number, 1.0);
    EXPECT_DOUBLE_EQ(cues[1].number, 2.0);
}

TEST_F(CueTagTest, EditCueTag_MissingNumber_Rejected) {
    add(1.0, 100);
    EXPECT_FALSE(timeline->editCueTag(99.0, 5.0, 0, ""));
}

TEST_F(CueTagTest, ClearOnTimelineClear) {
    add(1.0, 100);
    add(2.0, 200);
    timeline->clear();
    EXPECT_TRUE(timeline->getCueTags().empty());
}

TEST_F(CueTagTest, FractionalNumbers_DistinctEntries) {
    add(1.0, 100);
    add(1.25, 125);
    add(1.5, 150);
    add(1.75, 175);

    const auto& cues = timeline->getCueTags();
    ASSERT_EQ(cues.size(), 4u);
    EXPECT_DOUBLE_EQ(cues[0].number, 1.0);
    EXPECT_DOUBLE_EQ(cues[1].number, 1.25);
    EXPECT_DOUBLE_EQ(cues[2].number, 1.5);
    EXPECT_DOUBLE_EQ(cues[3].number, 1.75);
}
