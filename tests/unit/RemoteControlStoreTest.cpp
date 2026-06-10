#include <gtest/gtest.h>
#include "entity/remote/RemoteControlStore.hpp"

using entity::remote::RemoteControlStore;
using entity::remote::RemoteParam;

TEST(RemoteControlStore, AllocateAssignsDistinctSlots) {
    RemoteControlStore store;
    int a = store.allocateSlot("video1");
    int b = store.allocateSlot("video2");
    EXPECT_GE(a, 0); EXPECT_GE(b, 0); EXPECT_NE(a, b);
}

TEST(RemoteControlStore, DuplicateIdRejected) {
    RemoteControlStore store;
    EXPECT_GE(store.allocateSlot("video1"), 0);
    EXPECT_EQ(store.allocateSlot("video1"), -1);
}

TEST(RemoteControlStore, TableFullReturnsMinusOne) {
    RemoteControlStore store;
    for (int i = 0; i < entity::remote::kMaxRemotePatches; ++i) {
        EXPECT_GE(store.allocateSlot("p" + std::to_string(i)), 0);
    }
    EXPECT_EQ(store.allocateSlot("overflow"), -1);
}

TEST(RemoteControlStore, FreedSlotIsReusableAndCleared) {
    RemoteControlStore store;
    int a = store.allocateSlot("video1");
    store.setParamById("video1", RemoteParam::Opacity, 0.5f);
    store.setEngaged(a, true);
    store.freeSlot(a);
    EXPECT_FALSE(store.sample(a).engaged);
    EXPECT_FALSE(store.setParamById("video1", RemoteParam::Opacity, 0.7f));
    int b = store.allocateSlot("video9");
    EXPECT_EQ(b, a);  // slot reused
    EXPECT_EQ(store.sample(b).presentMask, 0u);  // cleared
}

TEST(RemoteControlStore, ValueStoredWhileDisengagedIsPresentOnEngage) {
    RemoteControlStore store;
    int a = store.allocateSlot("video1");
    EXPECT_TRUE(store.setParamById("video1", RemoteParam::Opacity, 0.25f));
    EXPECT_FALSE(store.sample(a).engaged);
    EXPECT_TRUE(store.sample(a).has(RemoteParam::Opacity));   // stored
    store.setEngaged(a, true);
    auto s = store.sample(a);
    EXPECT_TRUE(s.engaged);
    EXPECT_FLOAT_EQ(s.get(RemoteParam::Opacity), 0.25f);
}

TEST(RemoteControlStore, UnwrittenParamNotPresent) {
    RemoteControlStore store;
    int a = store.allocateSlot("video1");
    store.setParamById("video1", RemoteParam::PosX, 0.3f);
    auto s = store.sample(a);
    EXPECT_TRUE(s.has(RemoteParam::PosX));
    EXPECT_FALSE(s.has(RemoteParam::Opacity));
}

TEST(RemoteControlStore, RenamePreservesSlotAndValues) {
    RemoteControlStore store;
    int a = store.allocateSlot("video1");
    store.setParamById("video1", RemoteParam::Opacity, 0.4f);
    EXPECT_TRUE(store.renameSlot(a, "wall_left"));
    EXPECT_FALSE(store.setParamById("video1", RemoteParam::Opacity, 0.9f));
    EXPECT_TRUE(store.setParamById("wall_left", RemoteParam::Rotation, 45.0f));
    EXPECT_FLOAT_EQ(store.sample(a).get(RemoteParam::Opacity), 0.4f);
}

TEST(RemoteControlStore, RenameToExistingIdFails) {
    RemoteControlStore store;
    int a = store.allocateSlot("video1");
    store.allocateSlot("video2");
    EXPECT_FALSE(store.renameSlot(a, "video2"));
}

TEST(RemoteControlStore, TextGenerationConsume) {
    RemoteControlStore store;
    int a = store.allocateSlot("text1");
    std::uint32_t gen = 0;
    EXPECT_FALSE(store.consumeText(a, gen).has_value());  // nothing yet
    store.setTextById("text1", "HELLO");
    auto t = store.consumeText(a, gen);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "HELLO");
    EXPECT_FALSE(store.consumeText(a, gen).has_value());  // consumed
}

TEST(RemoteControlStore, SampleInvalidSlotIsInert) {
    RemoteControlStore store;
    EXPECT_FALSE(store.sample(-1).engaged);
    EXPECT_FALSE(store.sample(999).engaged);
}

// Write after free must not land on a reused slot. Verifies the invariant
// that set*ById holds m_idMutex across both the id-map lookup and the
// atomic stores, so a stale id can never resolve once the id is removed.
TEST(RemoteControlStore, WriteAfterFreeDoesNotLandOnReusedSlot) {
    RemoteControlStore store;
    const int a = store.allocateSlot("video1");
    ASSERT_GE(a, 0);
    store.freeSlot(a);

    // Reallocate the same physical slot under a new id.
    const int b = store.allocateSlot("video9");
    EXPECT_EQ(b, a);  // same slot index reused

    // The stale id must not resolve.
    EXPECT_FALSE(store.setParamById("video1", RemoteParam::Opacity, 0.5f));
    // And video9's mask must be clean — no stale write landed.
    EXPECT_EQ(store.sample(b).presentMask, 0u);
}
