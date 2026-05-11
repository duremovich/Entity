#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/TimelineTrack.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {

// Standalone Timeline + registry, no Engine. Exercises the snapshot/restore
// helpers DeleteClipCommand wraps. The clip-created callback that the
// command relies on for decoder + GPU resource setup is intentionally NOT
// invoked here -- restoreDeletedClip fires it but a unit test under bare
// Timeline can't validate that side. The user-visible "did the clip come
// back with the same component values" check belongs at this level.
class DeleteClipUndoTest : public ::testing::Test {
protected:
    entt::registry              registry;
    std::unique_ptr<Timeline>   timeline;
    entt::entity                track{entt::null};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        track = timeline->createTrack("Track 1");
    }

    entt::entity buildClip(FrameNumber start, FrameNumber duration) {
        entt::entity e = registry.create();
        auto& clip = registry.emplace<Clip>(e);
        clip.filepath        = "C:/fake/clip.mov";
        clip.mediaType       = MediaType::VideoProRes4444;
        clip.startFrame      = start;
        clip.duration        = duration;
        clip.mediaStartFrame = 5;
        clip.mediaOutFrame   = 95;
        clip.totalMediaFrames = 100;
        clip.framerate       = 24.0;
        clip.playbackMode    = PlaybackMode::Loop;
        clip.sectionBehavior = SectionBehavior::Locked;
        clip.width           = 1920;
        clip.height          = 1080;
        clip.hasAlpha        = true;
        clip.frameBlending   = true;

        auto& tr = registry.emplace<Transform>(e);
        tr.position = {1.5f, -2.0f, 0.5f};
        tr.rotation = {10.0f, 20.0f, 30.0f};
        tr.scale    = {1.25f, 1.5f, 1.0f};

        auto& ml = registry.emplace<MediaLayer>(e);
        ml.zOrder    = 7;
        ml.opacity   = 0.42f;
        ml.blendMode = BlendMode::Add;
        ml.visible   = false;

        auto& vt = registry.emplace<VideoTexture>(e);
        vt.width  = 1920;
        vt.height = 1080;
        vt.descriptorSlot = 13;  // Captured w/h only, NOT the slot.

        registry.emplace<FrameBuffer>(e);

        auto& ap = registry.emplace<AnimatedProperties>(e);
        ap.addKeyframe(AnimatableProperty::Opacity, 0,  0.0f);
        ap.addKeyframe(AnimatableProperty::Opacity, 30, 1.0f);

        registry.get<TimelineTrack>(track).clips.push_back(e);
        return e;
    }
};

}  // namespace

TEST_F(DeleteClipUndoTest, SnapshotCapturesAllRelevantState) {
    auto clipEnt = buildClip(60, 120);

    auto snap = timeline->snapshotClipForDelete(clipEnt);

    ASSERT_TRUE(snap.valid());
    EXPECT_EQ(static_cast<uint32_t>(snap.trackEntity), static_cast<uint32_t>(track));
    EXPECT_EQ(snap.filepath, "C:/fake/clip.mov");
    EXPECT_EQ(snap.mediaType, MediaType::VideoProRes4444);
    EXPECT_EQ(snap.startFrame, 60);
    EXPECT_EQ(snap.duration, 120);
    EXPECT_EQ(snap.mediaStartFrame, 5);
    EXPECT_EQ(snap.mediaOutFrame, 95);
    EXPECT_EQ(snap.totalMediaFrames, 100);
    EXPECT_DOUBLE_EQ(snap.framerate, 24.0);
    EXPECT_EQ(snap.playbackMode, PlaybackMode::Loop);
    EXPECT_EQ(snap.sectionBehavior, SectionBehavior::Locked);
    EXPECT_EQ(snap.width, 1920u);
    EXPECT_EQ(snap.height, 1080u);
    EXPECT_TRUE(snap.hasAlpha);
    EXPECT_TRUE(snap.frameBlending);

    EXPECT_TRUE(snap.hadTransform);
    EXPECT_FLOAT_EQ(snap.transform.position.x, 1.5f);
    EXPECT_FLOAT_EQ(snap.transform.rotation.y, 20.0f);
    EXPECT_FLOAT_EQ(snap.transform.scale.z, 1.0f);

    EXPECT_TRUE(snap.hadMediaLayer);
    EXPECT_EQ(snap.mediaLayer.zOrder, 7u);
    EXPECT_FLOAT_EQ(snap.mediaLayer.opacity, 0.42f);
    EXPECT_EQ(snap.mediaLayer.blendMode, BlendMode::Add);
    EXPECT_FALSE(snap.mediaLayer.visible);

    EXPECT_TRUE(snap.hadVideoTexture);
    EXPECT_EQ(snap.videoTexWidth, 1920u);
    EXPECT_EQ(snap.videoTexHeight, 1080u);

    EXPECT_TRUE(snap.hadFrameBuffer);
    EXPECT_TRUE(snap.hadAnimatedProperties);
    ASSERT_EQ(snap.animatedProperties.tracks.size(), 1u);
    EXPECT_EQ(snap.animatedProperties.tracks[0].keyframes.size(), 2u);
}

TEST_F(DeleteClipUndoTest, SnapshotInvalidForUnknownEntity) {
    entt::entity orphan = registry.create();
    auto snap = timeline->snapshotClipForDelete(orphan);
    EXPECT_FALSE(snap.valid());
}

TEST_F(DeleteClipUndoTest, SnapshotInvalidForClipNotInAnyTrack) {
    entt::entity floating = registry.create();
    registry.emplace<Clip>(floating);
    auto snap = timeline->snapshotClipForDelete(floating);
    EXPECT_FALSE(snap.valid());
}

TEST_F(DeleteClipUndoTest, RestoreRoundTripsAllComponentValues) {
    auto clipEnt = buildClip(60, 120);
    const auto originalEntityId = clipEnt;

    auto snap = timeline->snapshotClipForDelete(clipEnt);
    ASSERT_TRUE(snap.valid());

    timeline->deleteClip(clipEnt);
    ASSERT_FALSE(registry.valid(clipEnt));
    EXPECT_EQ(registry.get<TimelineTrack>(track).clips.size(), 0u);

    entt::entity restored = timeline->restoreDeletedClip(snap);
    ASSERT_TRUE(restored != entt::null);

    // Fresh entity ID expected — we don't promise ID stability across delete.
    EXPECT_NE(static_cast<uint32_t>(restored), static_cast<uint32_t>(originalEntityId));
    ASSERT_TRUE(registry.valid(restored));

    const auto& trackComp = registry.get<TimelineTrack>(track);
    ASSERT_EQ(trackComp.clips.size(), 1u);
    EXPECT_EQ(static_cast<uint32_t>(trackComp.clips.front()), static_cast<uint32_t>(restored));

    const auto& c = registry.get<Clip>(restored);
    EXPECT_EQ(c.filepath, "C:/fake/clip.mov");
    EXPECT_EQ(c.startFrame, 60);
    EXPECT_EQ(c.duration, 120);
    EXPECT_EQ(c.playbackMode, PlaybackMode::Loop);
    EXPECT_EQ(c.sectionBehavior, SectionBehavior::Locked);
    EXPECT_TRUE(c.hasAlpha);
    EXPECT_FALSE(c.loaded);    // Engine's clip-created callback re-opens.
    EXPECT_FALSE(c.decoding);

    const auto& tr = registry.get<Transform>(restored);
    EXPECT_FLOAT_EQ(tr.position.x, 1.5f);
    EXPECT_TRUE(tr.dirty);     // Forced so renderer recomputes the matrix.

    const auto& ml = registry.get<MediaLayer>(restored);
    EXPECT_EQ(ml.zOrder, 7u);
    EXPECT_FLOAT_EQ(ml.opacity, 0.42f);
    EXPECT_FALSE(ml.visible);

    const auto& vt = registry.get<VideoTexture>(restored);
    EXPECT_EQ(vt.width, 1920u);
    EXPECT_EQ(vt.height, 1080u);
    EXPECT_EQ(vt.descriptorSlot, UINT32_MAX);  // Sentinel: needs fresh slot.

    EXPECT_TRUE(registry.all_of<FrameBuffer>(restored));
    EXPECT_TRUE(registry.all_of<AnimatedProperties>(restored));
    ASSERT_EQ(registry.get<AnimatedProperties>(restored).tracks.size(), 1u);
    EXPECT_EQ(registry.get<AnimatedProperties>(restored).tracks[0].keyframes.size(), 2u);

    EXPECT_EQ(static_cast<uint32_t>(timeline->getSelectedClip()),
              static_cast<uint32_t>(restored));
}

TEST_F(DeleteClipUndoTest, RestoreInsertsInStartFrameOrder) {
    // Build three clips in start-frame order, snapshot the middle, delete it,
    // restore, and confirm the track's clip vector keeps start-frame ordering.
    auto a = buildClip(0,   100);
    auto b = buildClip(150, 100);
    auto c = buildClip(300, 100);

    auto snap = timeline->snapshotClipForDelete(b);
    ASSERT_TRUE(snap.valid());
    timeline->deleteClip(b);

    auto restoredB = timeline->restoreDeletedClip(snap);
    ASSERT_TRUE(restoredB != entt::null);

    const auto& trackClips = registry.get<TimelineTrack>(track).clips;
    ASSERT_EQ(trackClips.size(), 3u);
    EXPECT_EQ(static_cast<uint32_t>(trackClips[0]), static_cast<uint32_t>(a));
    EXPECT_EQ(static_cast<uint32_t>(trackClips[1]), static_cast<uint32_t>(restoredB));
    EXPECT_EQ(static_cast<uint32_t>(trackClips[2]), static_cast<uint32_t>(c));
}

TEST_F(DeleteClipUndoTest, RestoreFailsGracefullyWhenTrackGone) {
    auto clipEnt = buildClip(0, 100);
    auto snap = timeline->snapshotClipForDelete(clipEnt);
    ASSERT_TRUE(snap.valid());
    timeline->deleteClip(clipEnt);

    // Simulate the track itself being removed before the user hit undo --
    // restore should refuse rather than crash.
    registry.destroy(track);

    EXPECT_TRUE(timeline->restoreDeletedClip(snap) == entt::null);
}

TEST_F(DeleteClipUndoTest, RestoreFiresClipCreatedCallback) {
    auto clipEnt = buildClip(0, 100);
    auto snap = timeline->snapshotClipForDelete(clipEnt);
    ASSERT_TRUE(snap.valid());
    timeline->deleteClip(clipEnt);

    int callbackCount = 0;
    entt::entity callbackEntity = entt::null;
    std::string  callbackPath;
    timeline->setClipCreatedCallback(
        [&](entt::entity e, const std::string& p) {
            ++callbackCount;
            callbackEntity = e;
            callbackPath   = p;
        });

    auto restored = timeline->restoreDeletedClip(snap);
    ASSERT_TRUE(restored != entt::null);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(static_cast<uint32_t>(callbackEntity), static_cast<uint32_t>(restored));
    EXPECT_EQ(callbackPath, "C:/fake/clip.mov");
}
