#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <entt/entt.hpp>

using namespace entity;

// DeleteClipsCommand (Phase 12) — group delete with single-step undo.
//
// execute()/undo() take an Engine& but only call engine.getTimeline(), and
// constructing a real Engine pulls in D3D12 + windowing. So, mirroring the
// existing command-test convention (RenameTrackCommandTests, DeleteClipUndoTests):
//   - JSON shape is tested on the command directly.
//   - The execute/undo SEQUENCE is exercised against a bare Timeline using the
//     exact helpers the command calls (snapshotClipForDelete -> deleteClip;
//     restoreDeletedClip), which is where all the group-delete behavior lives.
namespace {

class DeleteClipsTest : public ::testing::Test {
protected:
    entt::registry            registry;
    std::unique_ptr<Timeline> timeline;
    entt::entity              trackA{entt::null};
    entt::entity              trackB{entt::null};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        trackA = timeline->createTrack("Track A");
        trackB = timeline->createTrack("Track B");
    }

    entt::entity addClip(entt::entity track, FrameNumber start, FrameNumber dur) {
        entt::entity e = registry.create();
        auto& clip = registry.emplace<Clip>(e);
        clip.filepath   = "C:/fake/clip.mov";
        clip.startFrame = start;
        clip.duration   = dur;
        clip.framerate  = 30.0;
        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Clip;
        lay.startFrame = start;
        lay.duration   = dur;
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }

    entt::entity addSignal(entt::entity track, FrameNumber start, FrameNumber dur) {
        entt::entity e = registry.create();
        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Signal;
        lay.startFrame = start;
        lay.duration   = dur;
        registry.emplace<SignalLayer>(e);
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }

    // Mirror of DeleteClipsCommand::execute over a bare Timeline: snapshot all
    // (skipping un-snapshottable kinds), delete the snapshottable ones.
    std::vector<Timeline::DeletedClipSnapshot>
    groupDelete(const std::vector<entt::entity>& targets, int& skipped) {
        skipped = 0;
        std::vector<Timeline::DeletedClipSnapshot> snaps;
        std::vector<entt::entity> toDelete;
        for (entt::entity e : targets) {
            auto s = timeline->snapshotClipForDelete(e);
            if (!s.valid()) { ++skipped; continue; }
            snaps.push_back(std::move(s));
            toDelete.push_back(e);
        }
        for (entt::entity e : toDelete) timeline->deleteClip(e);
        return snaps;
    }
};

}  // namespace

// ---- JSON shape --------------------------------------------------------------

TEST(DeleteClipsCommandJson, GetTypeName) {
    DeleteClipsCommand cmd;
    EXPECT_STREQ(cmd.getTypeName(), "DeleteClips");
}

TEST(DeleteClipsCommandJson, ToJsonStoresTrackLayerPairs) {
    DeleteClipsCommand cmd(std::vector<DeleteClipsCommand::TrackClip>{{0, 1}, {2, 0}});
    auto j = cmd.toJson();
    EXPECT_EQ(j["type"], "DeleteClips");
    ASSERT_TRUE(j.contains("clips"));
    ASSERT_EQ(j["clips"].size(), 2u);
    EXPECT_EQ(j["clips"][0]["track"], 0);
    EXPECT_EQ(j["clips"][0]["layer"], 1);
    EXPECT_EQ(j["clips"][1]["track"], 2);
    EXPECT_EQ(j["clips"][1]["layer"], 0);
}

TEST(DeleteClipsCommandJson, FromJsonRoundTrip) {
    nlohmann::json j = {{"type", "DeleteClips"},
                        {"clips", {{{"track", 1}, {"layer", 3}},
                                   {{"track", 0}, {"layer", 0}}}}};
    auto cmd = DeleteClipsCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    auto out = cmd->toJson();
    ASSERT_EQ(out["clips"].size(), 2u);
    EXPECT_EQ(out["clips"][0]["track"], 1);
    EXPECT_EQ(out["clips"][0]["layer"], 3);
    EXPECT_EQ(out["clips"][1]["track"], 0);
    EXPECT_EQ(out["clips"][1]["layer"], 0);
}

TEST(DeleteClipsCommandJson, FromJsonEmptyWhenNoClips) {
    nlohmann::json j = {{"type", "DeleteClips"}};
    auto cmd = DeleteClipsCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_TRUE(cmd->toJson()["clips"].empty());
}

// ---- Group snapshot / delete / restore (execute+undo sequence) ---------------

TEST_F(DeleteClipsTest, DeletesMultipleClipsAcrossTracks) {
    auto a = addClip(trackA, 0,   100);
    auto b = addClip(trackB, 200, 100);
    auto c = addClip(trackA, 400, 50);

    int skipped = 0;
    auto snaps = groupDelete({a, b, c}, skipped);

    EXPECT_EQ(skipped, 0);
    ASSERT_EQ(snaps.size(), 3u);
    EXPECT_FALSE(registry.valid(a));
    EXPECT_FALSE(registry.valid(b));
    EXPECT_FALSE(registry.valid(c));
    EXPECT_TRUE(registry.get<TimelineTrack>(trackA).layers.empty());
    EXPECT_TRUE(registry.get<TimelineTrack>(trackB).layers.empty());
}

TEST_F(DeleteClipsTest, UndoRestoresAllClipsToOriginalTracks) {
    auto a = addClip(trackA, 0,   100);
    auto b = addClip(trackB, 200, 100);

    int skipped = 0;
    auto snaps = groupDelete({a, b}, skipped);
    ASSERT_EQ(snaps.size(), 2u);

    // Undo: restore every snapshot (single logical step). Check validity via
    // registry.valid (not EXPECT_NE on entt::entity — that forces the
    // entt_traits<__int64> pretty-print instantiation).
    for (const auto& s : snaps) {
        entt::entity restored = timeline->restoreDeletedClip(s);
        EXPECT_TRUE(registry.valid(restored));
    }

    const auto& aLayers = registry.get<TimelineTrack>(trackA).layers;
    const auto& bLayers = registry.get<TimelineTrack>(trackB).layers;
    ASSERT_EQ(aLayers.size(), 1u);
    ASSERT_EQ(bLayers.size(), 1u);
    EXPECT_EQ(registry.get<Clip>(aLayers[0]).startFrame, 0);
    EXPECT_EQ(registry.get<Clip>(aLayers[0]).duration, 100);
    EXPECT_EQ(registry.get<Clip>(bLayers[0]).startFrame, 200);
    EXPECT_EQ(registry.get<Clip>(bLayers[0]).duration, 100);
}

TEST_F(DeleteClipsTest, SkipsSignalLayerButDeletesTheRest) {
    auto a   = addClip(trackA, 0, 100);
    auto sig = addSignal(trackA, 200, 50);  // not snapshottable -> skipped
    auto b   = addClip(trackB, 0, 100);

    int skipped = 0;
    auto snaps = groupDelete({a, sig, b}, skipped);

    EXPECT_EQ(skipped, 1);             // the Signal layer
    ASSERT_EQ(snaps.size(), 2u);       // a and b
    EXPECT_FALSE(registry.valid(a));
    EXPECT_FALSE(registry.valid(b));
    EXPECT_TRUE(registry.valid(sig));  // Signal survives
    // sig is still on its track.
    const auto& aLayers = registry.get<TimelineTrack>(trackA).layers;
    ASSERT_EQ(aLayers.size(), 1u);
    EXPECT_EQ(static_cast<uint32_t>(aLayers[0]), static_cast<uint32_t>(sig));
}

// ---- Timeline multi-select API (the set the command reads) -------------------

TEST_F(DeleteClipsTest, MultiSelectSetTracksMembershipAndPrimary) {
    auto a = addClip(trackA, 0, 100);
    auto b = addClip(trackB, 0, 100);

    // Compare entt::entity via uint32_t — a direct EXPECT_EQ on entt::entity
    // forces a gtest pretty-print that instantiates entt_traits<__int64>
    // (which doesn't exist) and fails to compile. Same convention as
    // RippleTimeTests / DeleteClipUndoTests.
    timeline->setSelectedClip(a);                 // resets set to {a}
    EXPECT_TRUE(timeline->isSelected(a));
    EXPECT_FALSE(timeline->isSelected(b));
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()), static_cast<uint32_t>(a));

    timeline->toggleSelected(b);                  // {a, b}, primary = b
    EXPECT_TRUE(timeline->isSelected(a));
    EXPECT_TRUE(timeline->isSelected(b));
    EXPECT_EQ(timeline->getSelectedClips().size(), 2u);
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()), static_cast<uint32_t>(b));

    timeline->toggleSelected(b);                  // back to {a}, primary = a
    EXPECT_FALSE(timeline->isSelected(b));
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()), static_cast<uint32_t>(a));

    timeline->setSelection({a, b, a});            // dedup -> {a, b}, primary = b
    EXPECT_EQ(timeline->getSelectedClips().size(), 2u);
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()), static_cast<uint32_t>(b));

    timeline->clearSelection();
    EXPECT_TRUE(timeline->getSelectedClips().empty());
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()),
              static_cast<uint32_t>(entt::null));
}

// Fix B: Timeline::deleteClip must drop the deleted entity from the
// multi-select set and repoint the primary, so no stale id lingers (which an
// entt id-recycle could otherwise turn into a phantom highlight).
TEST_F(DeleteClipsTest, DeleteClipPrunesSelectionSet) {
    auto a = addClip(trackA, 0, 100);
    auto b = addClip(trackB, 0, 100);
    auto c = addClip(trackA, 200, 100);

    timeline->setSelection({a, b, c});  // primary = c
    EXPECT_EQ(timeline->getSelectedClips().size(), 3u);

    // Delete a non-primary member -> dropped from the set, primary unchanged.
    timeline->deleteClip(b);
    EXPECT_FALSE(timeline->isSelected(b));
    EXPECT_EQ(timeline->getSelectedClips().size(), 2u);
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()), static_cast<uint32_t>(c));

    // Delete the primary -> dropped + primary repointed to the new last member.
    timeline->deleteClip(c);
    EXPECT_FALSE(timeline->isSelected(c));
    ASSERT_EQ(timeline->getSelectedClips().size(), 1u);
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()), static_cast<uint32_t>(a));

    // Delete the last member -> set empty, primary null.
    timeline->deleteClip(a);
    EXPECT_TRUE(timeline->getSelectedClips().empty());
    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()),
              static_cast<uint32_t>(entt::null));
}
