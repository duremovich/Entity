// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#include <gtest/gtest.h>
#include <entt/entt.hpp>

#include "entity/timeline/Timeline.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/TimelineTrack.hpp"

using namespace entity;

namespace {

// Mirror the SetUp pattern from ObjectAnimationLayerTests.cpp: bare registry +
// Timeline. No Engine instance needed — we call the factory directly on the
// registry to stay fast and dependency-free.
class SignalLayerTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;
    entt::entity track{};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        track    = timeline->createTrack("Track 0");
    }

    // Minimal factory mirroring Engine::createSignalLayer — used so the test
    // doesn't depend on the full Engine stack (which requires D3D12 + GLFW).
    entt::entity createSignalLayer(int trackIndex, FrameNumber start, FrameNumber dur) {
        auto* tt = registry.try_get<TimelineTrack>(track);
        if (!tt) return entt::null;

        entt::entity e = registry.create();

        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Signal;
        lay.startFrame = start;
        lay.duration   = dur;
        lay.trackIndex = static_cast<uint32_t>(trackIndex);

        registry.emplace<SignalLayer>(e);
        registry.emplace<AnimatedProperties>(e);

        tt->layers.push_back(e);
        return e;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Archetype assertions

TEST_F(SignalLayerTest, CreateEmplacesArchetype) {
    const entt::entity e = createSignalLayer(0, 10, 60);
    ASSERT_TRUE(registry.valid(e)) << "createSignalLayer must return a valid entity";

    // Must have Layer with Kind::Signal
    ASSERT_TRUE(registry.all_of<Layer>(e));
    EXPECT_EQ(registry.get<Layer>(e).kind, Layer::Kind::Signal);
    EXPECT_EQ(registry.get<Layer>(e).startFrame, FrameNumber{10});
    EXPECT_EQ(registry.get<Layer>(e).duration,   FrameNumber{60});

    // Must have SignalLayer
    EXPECT_TRUE(registry.all_of<SignalLayer>(e));

    // Must have AnimatedProperties
    EXPECT_TRUE(registry.all_of<AnimatedProperties>(e));

    // Must NOT be a content layer (no MediaLayer, no Transform)
    EXPECT_FALSE(registry.all_of<MediaLayer>(e))
        << "SignalLayer must NOT carry MediaLayer (not a content layer)";
    EXPECT_FALSE(registry.all_of<Transform>(e))
        << "SignalLayer must NOT carry Transform (not a content layer)";
}

TEST_F(SignalLayerTest, DefaultFieldValues) {
    const entt::entity e = createSignalLayer(0, 0, 30);
    ASSERT_TRUE(registry.valid(e));
    ASSERT_TRUE(registry.all_of<SignalLayer>(e));
    const auto& sig = registry.get<SignalLayer>(e);
    // Default mode is Momentary.
    EXPECT_EQ(sig.mode, SignalLayer::Mode::Momentary);
    // Default address is a valid non-empty string.
    EXPECT_FALSE(sig.address.empty());
    // Default transport is OSC.
    EXPECT_EQ(sig.transport, SignalLayer::Transport::OSC);
    // No args by default.
    EXPECT_TRUE(sig.args.empty());
}

TEST_F(SignalLayerTest, KindNameIsSignal) {
    EXPECT_STREQ(layerKindName(Layer::Kind::Signal), "Signal");
}

TEST_F(SignalLayerTest, SignalValuePropertyName) {
    EXPECT_STREQ(getPropertyName(AnimatableProperty::SignalValue), "Signal Value");
}
