#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {

// Helper: build a Timeline pre-populated with N named tracks, each holding
// one layer entity so we can verify Layer::trackIndex and MediaLayer::zOrder
// after insertions and deletions.
class TrackInsertTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
    }

    // Create a track and attach one layer entity with Layer + MediaLayer
    // so reindexTracks has something to update.
    entt::entity addTrackWithLayer() {
        entt::entity trackEnt = timeline->createTrack("T");
        auto& track = registry.get<TimelineTrack>(trackEnt);

        entt::entity layerEnt = registry.create();
        auto& lay = registry.emplace<Layer>(layerEnt);
        lay.startFrame = 0;
        lay.duration = 100;
        lay.trackIndex = track.trackIndex;
        lay.kind = Layer::Kind::Clip;

        auto& ml = registry.emplace<MediaLayer>(layerEnt);
        ml.zOrder = 1000u - track.trackIndex;

        track.addLayer(layerEnt);
        return trackEnt;
    }

    // Convenience accessors
    uint32_t trackIndex(entt::entity t) const {
        return registry.get<TimelineTrack>(t).trackIndex;
    }

    // Returns the trackIndex stored on the first layer of a track.
    uint32_t layerTrackIndex(entt::entity t) const {
        const auto& track = registry.get<TimelineTrack>(t);
        if (track.layers.empty()) return 9999u;
        return registry.get<Layer>(track.layers[0]).trackIndex;
    }

    // Returns the zOrder on the first layer of a track.
    uint32_t layerZOrder(entt::entity t) const {
        const auto& track = registry.get<TimelineTrack>(t);
        if (track.layers.empty()) return 9999u;
        return registry.get<MediaLayer>(track.layers[0]).zOrder;
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// insertTrackAt — basic index and sequence checks
// ---------------------------------------------------------------------------

TEST_F(TrackInsertTest, InsertAtBeginning_ShiftsAllDown) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();
    auto t2 = addTrackWithLayer();

    // Sanity: 3 tracks at indices 0,1,2
    ASSERT_EQ(trackIndex(t0), 0u);
    ASSERT_EQ(trackIndex(t1), 1u);
    ASSERT_EQ(trackIndex(t2), 2u);

    auto newTrack = timeline->insertTrackAt(0);

    // New track lands at index 0; old tracks shift down.
    EXPECT_EQ(trackIndex(newTrack), 0u);
    EXPECT_EQ(trackIndex(t0), 1u);
    EXPECT_EQ(trackIndex(t1), 2u);
    EXPECT_EQ(trackIndex(t2), 3u);
    EXPECT_EQ(timeline->getTrackCount(), 4u);
}

TEST_F(TrackInsertTest, InsertAboveMiddle_OnlyLaterTracksShift) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();
    auto t2 = addTrackWithLayer();

    auto newTrack = timeline->insertTrackAt(1);  // "Add Track Above" track 1

    EXPECT_EQ(trackIndex(newTrack), 1u);
    EXPECT_EQ(trackIndex(t0), 0u);   // before insertion point: unchanged
    EXPECT_EQ(trackIndex(t1), 2u);   // shifted
    EXPECT_EQ(trackIndex(t2), 3u);   // shifted
}

TEST_F(TrackInsertTest, InsertAtEnd_AppendsCorrectly) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();

    auto newTrack = timeline->insertTrackAt(2);  // "Add Track Below" last track

    EXPECT_EQ(trackIndex(t0), 0u);
    EXPECT_EQ(trackIndex(t1), 1u);
    EXPECT_EQ(trackIndex(newTrack), 2u);
}

TEST_F(TrackInsertTest, InsertBeyondEnd_Clamps) {
    auto t0 = addTrackWithLayer();

    // Inserting at index 999 should clamp to end (index 1).
    auto newTrack = timeline->insertTrackAt(999);

    EXPECT_EQ(trackIndex(t0), 0u);
    EXPECT_EQ(trackIndex(newTrack), 1u);
    EXPECT_EQ(timeline->getTrackCount(), 2u);
}

// ---------------------------------------------------------------------------
// Layer::trackIndex and MediaLayer::zOrder are updated on insert
// ---------------------------------------------------------------------------

TEST_F(TrackInsertTest, InsertAboveMiddle_LayerTrackIndexUpdated) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();
    auto t2 = addTrackWithLayer();

    timeline->insertTrackAt(1);

    // t0 is still at position 0 — Layer::trackIndex unchanged.
    EXPECT_EQ(layerTrackIndex(t0), 0u);
    // t1 shifted to position 2 — Layer::trackIndex must reflect that.
    EXPECT_EQ(layerTrackIndex(t1), 2u);
    EXPECT_EQ(layerTrackIndex(t2), 3u);
}

TEST_F(TrackInsertTest, InsertAboveMiddle_ZOrderUpdated) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();
    auto t2 = addTrackWithLayer();

    timeline->insertTrackAt(1);

    // zOrder = 1000 - trackIndex.
    EXPECT_EQ(layerZOrder(t0), 1000u);  // position 0
    EXPECT_EQ(layerZOrder(t1), 998u);   // position 2
    EXPECT_EQ(layerZOrder(t2), 997u);   // position 3
}

// ---------------------------------------------------------------------------
// deleteTrack — reindexes Layer::trackIndex and MediaLayer::zOrder
// ---------------------------------------------------------------------------

TEST_F(TrackInsertTest, DeleteMiddleTrack_ReindexesSuccessors) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();
    auto t2 = addTrackWithLayer();

    timeline->deleteTrack(t1);

    EXPECT_EQ(timeline->getTrackCount(), 2u);
    EXPECT_EQ(trackIndex(t0), 0u);
    EXPECT_EQ(trackIndex(t2), 1u);
    EXPECT_EQ(layerTrackIndex(t0), 0u);
    EXPECT_EQ(layerTrackIndex(t2), 1u);
    EXPECT_EQ(layerZOrder(t0), 1000u);
    EXPECT_EQ(layerZOrder(t2), 999u);
}

TEST_F(TrackInsertTest, DeleteFirstTrack_ReindexesAll) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();
    auto t2 = addTrackWithLayer();

    timeline->deleteTrack(t0);

    EXPECT_EQ(trackIndex(t1), 0u);
    EXPECT_EQ(trackIndex(t2), 1u);
    EXPECT_EQ(layerZOrder(t1), 1000u);
    EXPECT_EQ(layerZOrder(t2), 999u);
}

// ---------------------------------------------------------------------------
// Round-trip: insert then delete restores original sequence
// ---------------------------------------------------------------------------

TEST_F(TrackInsertTest, InsertThenDelete_RestoresSequence) {
    auto t0 = addTrackWithLayer();
    auto t1 = addTrackWithLayer();

    auto newTrack = timeline->insertTrackAt(1);
    ASSERT_EQ(trackIndex(newTrack), 1u);

    timeline->deleteTrack(newTrack);

    EXPECT_EQ(timeline->getTrackCount(), 2u);
    EXPECT_EQ(trackIndex(t0), 0u);
    EXPECT_EQ(trackIndex(t1), 1u);
    EXPECT_EQ(layerTrackIndex(t0), 0u);
    EXPECT_EQ(layerTrackIndex(t1), 1u);
    EXPECT_EQ(layerZOrder(t0), 1000u);
    EXPECT_EQ(layerZOrder(t1), 999u);
}
