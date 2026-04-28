#include "entity/core/SceneState.hpp"

#include <gtest/gtest.h>

#include <entt/entt.hpp>

using namespace entity;

namespace {

struct Position { float x, y; };

} // namespace

TEST(SceneState, DirectAccessReturnsSameRegistry) {
    entt::registry reg;
    SceneState s(reg);
    EXPECT_EQ(&s.registry(), &reg);
    EXPECT_EQ(&s.cregistry(), &reg);
    const SceneState& cs = s;
    EXPECT_EQ(&cs.registry(), &reg);
}

TEST(SceneState, ReadHandleExposesConstRegistry) {
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<Position>(e, 1.0f, 2.0f);

    SceneState s(reg);
    {
        auto h = s.read();
        const auto& r = h.registry();
        ASSERT_TRUE(r.all_of<Position>(e));
        EXPECT_FLOAT_EQ(r.get<Position>(e).x, 1.0f);
    }
}

TEST(SceneState, WriteHandleExposesMutableRegistry) {
    entt::registry reg;
    SceneState s(reg);
    entt::entity e;
    {
        auto h = s.write();
        e = h.registry().create();
        h.registry().emplace<Position>(e, 7.0f, 8.0f);
    }
    EXPECT_TRUE(reg.all_of<Position>(e));
    EXPECT_FLOAT_EQ(reg.get<Position>(e).y, 8.0f);
}

TEST(SceneState, MultipleReadHandlesStack) {
    entt::registry reg;
    SceneState s(reg);
    {
        auto h1 = s.read();
        auto h2 = s.read();
        auto h3 = s.read();
#ifndef NDEBUG
        EXPECT_EQ(s.activeReaders(), 3);
        EXPECT_EQ(s.activeWriters(), 0);
#endif
    }
#ifndef NDEBUG
    EXPECT_EQ(s.activeReaders(), 0);
#endif
}

TEST(SceneState, SequentialReadThenWriteThenReadIsClean) {
    entt::registry reg;
    SceneState s(reg);
    { auto r = s.read(); (void)r; }
    { auto w = s.write(); (void)w; }
    { auto r = s.read(); (void)r; }
#ifndef NDEBUG
    EXPECT_EQ(s.activeReaders(), 0);
    EXPECT_EQ(s.activeWriters(), 0);
#endif
}
