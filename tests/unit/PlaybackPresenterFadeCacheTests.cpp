// Phase D BLOCKER fix — verify PlaybackPresenter applies section-fade
// multipliers via a Renderer-local cache (consulted by CompositorSystem at
// draw time) and NEVER mutates MediaLayer.opacity in the registry. The
// prior implementation wrote `layer->opacity *= multiplier` directly,
// producing exponential decay across ticks for any clip without an
// enabled Opacity keyframe track to rewrite the value, AND violating the
// Director-writes / Renderer-reads boundary contract.
//
// These tests construct PlaybackPresenter with nullptr backends so the
// upload path short-circuits; the fade cache refresh runs before the
// short-circuit, so we can verify cache state directly.

#include <gtest/gtest.h>

#include "entity/bus/Message.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"

#include <entt/entt.hpp>

using namespace entity;

class PlaybackPresenterFadeCacheTest : public ::testing::Test {
protected:
    entt::registry registry;
    PlaybackPresenter presenter{registry, nullptr, nullptr};
};

TEST_F(PlaybackPresenterFadeCacheTest, UnknownEntityReturnsIdentity) {
    auto entity = registry.create();
    EXPECT_FLOAT_EQ(presenter.fadeMultiplier(entity), 1.0f);
}

TEST_F(PlaybackPresenterFadeCacheTest, BusPayloadPopulatesCacheForFadingClips) {
    auto fading = registry.create();
    auto identity = registry.create();

    bus::RenderFrame rf;
    rf.playState = TransportState::Playing;

    bus::ClipRenderState a;
    a.entity = static_cast<std::uint64_t>(fading);
    a.sectionFadeMultiplier = 0.42f;
    rf.activeClips.push_back(a);

    bus::ClipRenderState b;
    b.entity = static_cast<std::uint64_t>(identity);
    b.sectionFadeMultiplier = 1.0f;  // not in fade window
    rf.activeClips.push_back(b);

    presenter.refreshFadeMultiplierCache(rf);

    EXPECT_FLOAT_EQ(presenter.fadeMultiplier(fading),   0.42f);
    // Identity multipliers are intentionally NOT cached -- the lookup
    // already returns 1.0f as the fallback, and skipping them keeps the
    // common no-fade case zero-cost.
    EXPECT_FLOAT_EQ(presenter.fadeMultiplier(identity), 1.0f);
}

TEST_F(PlaybackPresenterFadeCacheTest, RefreshClearsStaleEntries) {
    auto entity = registry.create();

    bus::RenderFrame rfFading;
    bus::ClipRenderState fading;
    fading.entity = static_cast<std::uint64_t>(entity);
    fading.sectionFadeMultiplier = 0.5f;
    rfFading.activeClips.push_back(fading);
    presenter.refreshFadeMultiplierCache(rfFading);
    EXPECT_FLOAT_EQ(presenter.fadeMultiplier(entity), 0.5f);

    // Next tick: the clip exited its fade window. Bus payload either
    // omits the clip entirely (it left the active set) or reports
    // multiplier == 1.0. Either way the cache must drop the stale 0.5.
    bus::RenderFrame rfClean;  // empty -> presenter.fadeMultiplier should fall back to 1.0
    presenter.refreshFadeMultiplierCache(rfClean);
    EXPECT_FLOAT_EQ(presenter.fadeMultiplier(entity), 1.0f);
}

TEST_F(PlaybackPresenterFadeCacheTest, PresenterDoesNotMutateMediaLayerOpacity) {
    // Regression guard for the BLOCKER. Construct a clip with
    // MediaLayer.opacity = 1.0; refresh the cache with a fading bus
    // payload; the registry-side opacity must remain 1.0. Five times
    // for good measure -- the prior implementation's exponential decay
    // would have walked it down to ~0.03 by tick 5 with multiplier 0.5.
    auto entity = registry.create();
    auto& layer = registry.emplace<MediaLayer>(entity);
    layer.opacity = 1.0f;

    bus::RenderFrame rf;
    bus::ClipRenderState clip;
    clip.entity = static_cast<std::uint64_t>(entity);
    clip.sectionFadeMultiplier = 0.5f;
    rf.activeClips.push_back(clip);

    for (int tick = 0; tick < 5; ++tick) {
        presenter.refreshFadeMultiplierCache(rf);
        EXPECT_FLOAT_EQ(layer.opacity, 1.0f) << "tick " << tick;
    }
    // The multiplier itself stays available for CompositorSystem to read.
    EXPECT_FLOAT_EQ(presenter.fadeMultiplier(entity), 0.5f);
}
