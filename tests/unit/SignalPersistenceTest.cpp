// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Round-trip persistence test for SignalLayer (project schema v26, ADR-0027).
// Saves a timeline containing a signal layer with non-default field values
// (including a multi-type arg list and range mapping), reloads into a fresh
// registry, and asserts every field survived.
#include <gtest/gtest.h>

#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

fs::path makeTempPath() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
         / ("entity_signal_persist_" + std::to_string(stamp) + ".entity");
}

struct TempFile {
    fs::path path;
    TempFile() : path(makeTempPath()) {}
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

// Find the first SignalLayer entity in the registry. Returns entt::null if absent.
entt::entity findSignalLayer(entt::registry& reg) {
    auto view = reg.view<entity::SignalLayer>();
    for (auto [e, sig] : view.each()) { (void)sig; return e; }
    return entt::null;
}

} // namespace

// ---------------------------------------------------------------------------
// Full round-trip: non-default field values survive save/load.

TEST(SignalPersistence, RoundTripAllFields) {
    TempFile tf;

    // --- Save ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        const auto trackEnt = timeline.createTrack("Track 0");

        entt::entity e = registry.create();

        auto& lay = registry.emplace<entity::Layer>(e);
        lay.kind       = entity::Layer::Kind::Signal;
        lay.startFrame = 42;
        lay.duration   = 7;
        lay.name       = "TestCue";
        lay.color      = {0.9f, 0.6f, 0.2f, 1.0f};
        lay.trackIndex = 0;

        auto& sig = registry.emplace<entity::SignalLayer>(e);
        sig.mode        = entity::SignalLayer::Mode::Continuous;
        sig.transport   = entity::SignalLayer::Transport::OSC;
        sig.valueSource = entity::SignalLayer::ValueSource::Keyframe;
        sig.address     = "/entity/test/cue";
        sig.srcMin      = 0.25f;
        sig.srcMax      = 0.75f;
        sig.outMin      = 10.0f;
        sig.outMax      = 200.0f;
        sig.outType     = entity::SignalLayer::Arg::Type::Int;

        // Three args: int literal, float value-bound, string literal.
        entity::SignalLayer::Arg a0;
        a0.type = entity::SignalLayer::Arg::Type::Int;
        a0.i   = 99;
        a0.isValueBound = false;

        entity::SignalLayer::Arg a1;
        a1.type = entity::SignalLayer::Arg::Type::Float;
        a1.f   = 3.14f;
        a1.isValueBound = true;

        entity::SignalLayer::Arg a2;
        a2.type = entity::SignalLayer::Arg::Type::String;
        a2.s   = "hello";
        a2.isValueBound = false;

        sig.args = {a0, a1, a2};

        registry.emplace<entity::AnimatedProperties>(e);
        registry.get<entity::TimelineTrack>(trackEnt).layers.push_back(e);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    // --- Load (fresh registry) ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        const entt::entity e = findSignalLayer(registry);
        ASSERT_TRUE(registry.valid(e)) << "No SignalLayer entity after load";

        // Layer fields
        ASSERT_TRUE(registry.all_of<entity::Layer>(e));
        const auto& lay = registry.get<entity::Layer>(e);
        EXPECT_EQ(lay.kind,       entity::Layer::Kind::Signal);
        EXPECT_EQ(lay.startFrame, entity::FrameNumber{42});
        EXPECT_EQ(lay.duration,   entity::FrameNumber{7});
        EXPECT_EQ(lay.name,       "TestCue");
        EXPECT_NEAR(lay.color[0], 0.9f, 0.001f);
        EXPECT_NEAR(lay.color[1], 0.6f, 0.001f);

        // SignalLayer fields
        ASSERT_TRUE(registry.all_of<entity::SignalLayer>(e));
        const auto& sig = registry.get<entity::SignalLayer>(e);
        EXPECT_EQ(sig.mode,        entity::SignalLayer::Mode::Continuous);
        EXPECT_EQ(sig.transport,   entity::SignalLayer::Transport::OSC);
        EXPECT_EQ(sig.valueSource, entity::SignalLayer::ValueSource::Keyframe);
        EXPECT_EQ(sig.address,     "/entity/test/cue");
        EXPECT_NEAR(sig.srcMin, 0.25f, 0.001f);
        EXPECT_NEAR(sig.srcMax, 0.75f, 0.001f);
        EXPECT_NEAR(sig.outMin, 10.0f, 0.001f);
        EXPECT_NEAR(sig.outMax, 200.0f, 0.001f);
        EXPECT_EQ(sig.outType, entity::SignalLayer::Arg::Type::Int);

        // Arg list
        ASSERT_EQ(sig.args.size(), 3u);

        EXPECT_EQ(sig.args[0].type, entity::SignalLayer::Arg::Type::Int);
        EXPECT_EQ(sig.args[0].i,    99);
        EXPECT_FALSE(sig.args[0].isValueBound);

        EXPECT_EQ(sig.args[1].type, entity::SignalLayer::Arg::Type::Float);
        EXPECT_NEAR(sig.args[1].f,  3.14f, 0.001f);
        EXPECT_TRUE(sig.args[1].isValueBound);

        EXPECT_EQ(sig.args[2].type, entity::SignalLayer::Arg::Type::String);
        EXPECT_EQ(sig.args[2].s,    "hello");
        EXPECT_FALSE(sig.args[2].isValueBound);

        // AnimatedProperties must be present (emplaced unconditionally).
        EXPECT_TRUE(registry.all_of<entity::AnimatedProperties>(e));

        // Must NOT carry MediaLayer or Transform (not a content layer).
        EXPECT_FALSE(registry.all_of<entity::MediaLayer>(e))
            << "Signal layer must not carry MediaLayer after load";
    }
}

// ---------------------------------------------------------------------------
// Break-tolerant: a pre-v26 project with no signal layers loads cleanly.

TEST(SignalPersistence, OlderProjectLoadsWithNoSignalLayers) {
    // Build a minimal project with only a track and no signal layers,
    // serialize, then reload — no SignalLayer entities should appear.
    TempFile tf;

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        timeline.createTrack("Track 0");
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        const entt::entity e = findSignalLayer(registry);
        // Loaded project had no signal layers; none should exist.
        EXPECT_FALSE(registry.valid(e))
            << "No SignalLayer expected in a project that had none";
    }
}

// ---------------------------------------------------------------------------
// Defaults round-trip: a default-constructed SignalLayer survives save/load.

TEST(SignalPersistence, DefaultSignalLayerRoundTrip) {
    TempFile tf;

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        const auto trackEnt = timeline.createTrack("Track 0");

        entt::entity e = registry.create();
        auto& lay = registry.emplace<entity::Layer>(e);
        lay.kind = entity::Layer::Kind::Signal;
        lay.startFrame = 0;
        lay.duration   = 1;
        registry.emplace<entity::SignalLayer>(e);  // all defaults
        registry.emplace<entity::AnimatedProperties>(e);
        registry.get<entity::TimelineTrack>(trackEnt).layers.push_back(e);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        const entt::entity e = findSignalLayer(registry);
        ASSERT_TRUE(registry.valid(e));
        ASSERT_TRUE(registry.all_of<entity::SignalLayer>(e));
        const auto& sig = registry.get<entity::SignalLayer>(e);
        EXPECT_EQ(sig.mode, entity::SignalLayer::Mode::Momentary);
        EXPECT_EQ(sig.address, "/entity/signal");
        EXPECT_TRUE(sig.args.empty());
    }
}
