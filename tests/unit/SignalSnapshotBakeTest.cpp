// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#include <gtest/gtest.h>
#include <entt/entt.hpp>

#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/timeline/Timeline.hpp"

using namespace entity;

namespace {

class SignalSnapshotBakeTest : public ::testing::Test {
protected:
    entt::registry          registry;
    Timeline                timeline{registry};
    PlaybackTimeAuthority   auth{registry, &timeline};

    entt::entity track{};

    void SetUp() override {
        track = timeline.createTrack("Track 0");
    }

    // Emplace a minimal Signal layer entity at the given timeline window.
    entt::entity addSignalLayer(SignalLayer::Mode mode,
                                FrameNumber start, FrameNumber duration) {
        entt::entity e = registry.create();

        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Signal;
        lay.startFrame = start;
        lay.duration   = duration;
        lay.trackIndex = 0;

        auto& sig = registry.emplace<SignalLayer>(e);
        sig.mode    = mode;
        sig.address = "/entity/signal/test";

        registry.emplace<AnimatedProperties>(e);

        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Continuous mode: active while playhead is inside [start, end).

TEST_F(SignalSnapshotBakeTest, ContinuousActiveWhileInWindow) {
    addSignalLayer(SignalLayer::Mode::Continuous, 10, 20);
    // start=10, end=30

    bus::SceneSnapshot snap;

    timeline.seekToFrame(9);
    auth.buildSceneSnapshot(snap);
    EXPECT_TRUE(snap.signalLayers.empty())
        << "frame 9: before window, should be absent";

    timeline.seekToFrame(10);
    auth.buildSceneSnapshot(snap);
    EXPECT_EQ(snap.signalLayers.size(), 1u)
        << "frame 10: start of window, should be present";
    EXPECT_EQ(snap.signalLayers[0].mode, 1)  // Continuous
        << "mode should be Continuous (1)";
    EXPECT_EQ(snap.signalLayers[0].address, "/entity/signal/test");

    timeline.seekToFrame(25);
    auth.buildSceneSnapshot(snap);
    EXPECT_EQ(snap.signalLayers.size(), 1u)
        << "frame 25: inside window, should be present";

    timeline.seekToFrame(30);
    auth.buildSceneSnapshot(snap);
    EXPECT_TRUE(snap.signalLayers.empty())
        << "frame 30: first frame past end, should be absent";
}

// ---------------------------------------------------------------------------
// Momentary mode: straddling window [start-2, end+2].
//
// Rationale: the headless loop runs at thousands of fps and cue-jumps advance
// the playhead by many frames per tick. Exact equality (== startFrame) would
// silently drop cues whenever a tick steps over startFrame. The ±2 window
// ensures the layer appears in the snapshot for skips like prev=start-1,
// cur=start+2. Phase 5 re-arms after the window exits so it fires exactly once.

TEST_F(SignalSnapshotBakeTest, MomentaryActiveInStraddlingWindow) {
    addSignalLayer(SignalLayer::Mode::Momentary, 15, 30);
    // startFrame=15, endFrame=45, window=[13, 47]

    bus::SceneSnapshot snap;

    // One frame before the window — absent.
    timeline.seekToFrame(12);
    auth.buildSceneSnapshot(snap);
    EXPECT_TRUE(snap.signalLayers.empty())
        << "frame 12: before window (start-3), should be absent";

    // At startFrame — present.
    timeline.seekToFrame(15);
    auth.buildSceneSnapshot(snap);
    EXPECT_EQ(snap.signalLayers.size(), 1u)
        << "frame 15: startFrame, should be present";
    EXPECT_EQ(snap.signalLayers[0].mode, 0)  // Momentary
        << "mode should be Momentary (0)";

    // Regression guard: start+1 must ALSO be present (guards against the
    // exact-equality bug where a tick that skips startFrame drops the cue).
    timeline.seekToFrame(16);
    auth.buildSceneSnapshot(snap);
    EXPECT_EQ(snap.signalLayers.size(), 1u)
        << "frame 16 (start+1): inside window, must be present "
           "(regression guard for single-tick skip-over)";

    // start+2: still inside window.
    timeline.seekToFrame(17);
    auth.buildSceneSnapshot(snap);
    EXPECT_EQ(snap.signalLayers.size(), 1u)
        << "frame 17 (start+2): boundary of window, must be present";

    // Well past endFrame+2: absent.
    timeline.seekToFrame(48);
    auth.buildSceneSnapshot(snap);
    EXPECT_TRUE(snap.signalLayers.empty())
        << "frame 48 (end+3): beyond window, should be absent";
}

// ---------------------------------------------------------------------------
// Baked fields: entity, startFrame, duration, address round-trip correctly.

TEST_F(SignalSnapshotBakeTest, BakedFieldsCorrect) {
    const entt::entity e = addSignalLayer(SignalLayer::Mode::Continuous, 5, 10);

    bus::SceneSnapshot snap;
    timeline.seekToFrame(7);
    auth.buildSceneSnapshot(snap);

    ASSERT_EQ(snap.signalLayers.size(), 1u);
    const auto& sl = snap.signalLayers[0];
    EXPECT_EQ(sl.entity,     static_cast<std::uint64_t>(e));
    EXPECT_EQ(sl.startFrame, FrameNumber{5});
    EXPECT_EQ(sl.duration,   FrameNumber{10});
    EXPECT_EQ(sl.address,    "/entity/signal/test");
}

// ---------------------------------------------------------------------------
// Args bake: a value-bound arg is reflected in valueBoundArgIndex.

TEST_F(SignalSnapshotBakeTest, ValueBoundArgIndexBaked) {
    entt::entity e = registry.create();
    {
        auto& lay = registry.emplace<Layer>(e);
        lay.kind = Layer::Kind::Signal; lay.startFrame = 0; lay.duration = 60;
        lay.trackIndex = 0;
    }
    auto& sig = registry.emplace<SignalLayer>(e);
    sig.mode    = SignalLayer::Mode::Continuous;
    sig.address = "/foo";
    // Arg 0: literal int, not bound.
    SignalLayer::Arg a0;
    a0.type = SignalLayer::Arg::Type::Int; a0.i = 7; a0.isValueBound = false;
    // Arg 1: float, value-bound.
    SignalLayer::Arg a1;
    a1.type = SignalLayer::Arg::Type::Float; a1.isValueBound = true;
    sig.args.push_back(a0);
    sig.args.push_back(a1);
    registry.emplace<AnimatedProperties>(e);
    registry.get<TimelineTrack>(track).layers.push_back(e);

    bus::SceneSnapshot snap;
    timeline.seekToFrame(0);
    auth.buildSceneSnapshot(snap);

    ASSERT_EQ(snap.signalLayers.size(), 1u);
    const auto& sl = snap.signalLayers[0];
    ASSERT_EQ(sl.args.size(), 2u);
    EXPECT_EQ(sl.valueBoundArgIndex, 1)
        << "second arg (index 1) is value-bound";
    // First arg carries literal int value.
    EXPECT_EQ(sl.args[0].i, 7);
}

// ---------------------------------------------------------------------------
// Serialization round-trip: SignalLayerSnapshot survives encode/decode.

TEST_F(SignalSnapshotBakeTest, SerializationRoundTrip) {
    addSignalLayer(SignalLayer::Mode::Continuous, 0, 60);

    bus::SceneSnapshot snap;
    timeline.seekToFrame(5);
    auth.buildSceneSnapshot(snap);
    ASSERT_EQ(snap.signalLayers.size(), 1u);

    // Wrap in a SceneSnapshot Message and round-trip.
    bus::Message msg = snap;
    auto bytes   = bus::serialize(msg);
    auto decoded = bus::deserialize(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<bus::SceneSnapshot>(*decoded));

    const auto& out = std::get<bus::SceneSnapshot>(*decoded);
    ASSERT_EQ(out.signalLayers.size(), 1u);
    EXPECT_EQ(out.signalLayers[0].address, "/entity/signal/test");
    EXPECT_EQ(out.signalLayers[0].mode, 1);  // Continuous
}
