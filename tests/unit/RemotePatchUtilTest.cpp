#include <gtest/gtest.h>
#include "entity/remote/RemotePatchUtil.hpp"

using namespace entity;

TEST(RemotePatchUtil, ValidIds) {
    EXPECT_TRUE(remote::isValidPatchId("video1"));
    EXPECT_TRUE(remote::isValidPatchId("wall_left_2"));
    EXPECT_FALSE(remote::isValidPatchId(""));
    EXPECT_FALSE(remote::isValidPatchId("Video1"));   // uppercase
    EXPECT_FALSE(remote::isValidPatchId("video 1"));  // space
    EXPECT_FALSE(remote::isValidPatchId("video/1"));  // slash
}

TEST(RemotePatchUtil, AutoIdSkipsTaken) {
    entt::registry reg;
    auto a = reg.create();
    reg.emplace<RemotePatch>(a, RemotePatch{"video1", false, 0, 0});
    auto b = reg.create();
    reg.emplace<Clip>(b);
    EXPECT_EQ(remote::makeAutoPatchId(reg, b), "video2");
}

TEST(RemotePatchUtil, KindBases) {
    entt::registry reg;
    auto t = reg.create();
    reg.emplace<TextLayerState>(t);
    EXPECT_EQ(remote::autoPatchBase(reg, t), std::string("text"));
    auto plain = reg.create();
    EXPECT_EQ(remote::autoPatchBase(reg, plain), std::string("layer"));
}

TEST(RemotePatchUtil, MuncherBase) {
    entt::registry reg;
    auto m = reg.create();
    reg.emplace<MunchersGameState>(m);
    EXPECT_EQ(remote::autoPatchBase(reg, m), std::string("muncher"));
}

TEST(RemotePatchUtil, PatchIdInUseDetectsExisting) {
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<RemotePatch>(e, RemotePatch{"wall_left", false, 0, 0});
    EXPECT_TRUE(remote::patchIdInUse(reg, "wall_left"));
    EXPECT_FALSE(remote::patchIdInUse(reg, "wall_right"));
}
