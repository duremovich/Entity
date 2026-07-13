#include <gtest/gtest.h>

#include "entity/timeline/Timeline.hpp"
#include "entity/timeline/TimelineWidget.hpp"

#include <entt/entt.hpp>

using namespace entity;

// The zoom ladder used to be pure frame counts {1,2,5,10,20,50,100,200,500}, so
// at 30fps no stop landed on a second boundary and you couldn't count seconds off
// the grid to space keyframes by eye. Stops are now frames OR seconds, and
// framesPerTick() is the single seam that resolves a seconds stop against the
// project frame rate — the ruler, the gridlines, and scrub snapping all read it.

namespace {

// Index of a stop by label, so the tests don't hard-code ladder positions.
int stopIndex(const char* label) {
    for (int i = 0; i < TimelineWidget::ZOOM_LEVEL_COUNT; ++i) {
        if (std::string(TimelineWidget::ZOOM_STOPS[i].label) == label) return i;
    }
    return -1;
}

}  // namespace

TEST(TimelineZoomLadder, FrameStopsAreFrameRateIndependent) {
    entt::registry registry;
    Timeline timeline(registry);
    TimelineWidget widget(&timeline);

    const int idx = stopIndex("10f");
    ASSERT_GE(idx, 0);
    widget.setZoomIndex(idx);

    for (double fps : {24.0, 25.0, 30.0, 59.94}) {
        timeline.setFrameRate(fps);
        EXPECT_EQ(widget.framesPerTick(), 10) << "at fps=" << fps;
    }
}

TEST(TimelineZoomLadder, SecondStopsResolveAgainstFrameRate) {
    entt::registry registry;
    Timeline timeline(registry);
    TimelineWidget widget(&timeline);

    const int oneSecond = stopIndex("1s");
    ASSERT_GE(oneSecond, 0);
    widget.setZoomIndex(oneSecond);

    timeline.setFrameRate(30.0);
    EXPECT_EQ(widget.framesPerTick(), 30);

    timeline.setFrameRate(24.0);
    EXPECT_EQ(widget.framesPerTick(), 24);

    // Drop-frame rates round to the nearest whole frame so the grid stays on
    // frame boundaries rather than drifting sub-frame.
    timeline.setFrameRate(29.97);
    EXPECT_EQ(widget.framesPerTick(), 30);

    timeline.setFrameRate(23.976);
    EXPECT_EQ(widget.framesPerTick(), 24);
}

TEST(TimelineZoomLadder, MultiSecondStopsScale) {
    entt::registry registry;
    Timeline timeline(registry);
    TimelineWidget widget(&timeline);
    timeline.setFrameRate(30.0);

    struct Expect { const char* label; int frames; };
    const Expect cases[] = {
        {"1s",   30},
        {"2s",   60},
        {"5s",  150},
        {"10s", 300},
        {"30s", 900},
        {"1m",  1800},
    };

    for (const auto& c : cases) {
        const int idx = stopIndex(c.label);
        ASSERT_GE(idx, 0) << c.label;
        widget.setZoomIndex(idx);
        EXPECT_EQ(widget.framesPerTick(), c.frames) << "stop " << c.label;
    }
}

// A 0 stride would hang the ruler and gridline tick loops (they step by
// framesPerTick()). Nothing should be able to drive it below 1.
TEST(TimelineZoomLadder, NeverReturnsZeroStride) {
    entt::registry registry;
    Timeline timeline(registry);
    TimelineWidget widget(&timeline);

    for (int i = 0; i < TimelineWidget::ZOOM_LEVEL_COUNT; ++i) {
        widget.setZoomIndex(i);
        for (double fps : {0.01, 1.0, 23.976, 30.0, 120.0}) {
            timeline.setFrameRate(fps);
            EXPECT_GE(widget.framesPerTick(), 1)
                << "stop " << TimelineWidget::ZOOM_STOPS[i].label << " at fps=" << fps;
        }
    }
}

TEST(TimelineZoomLadder, DefaultZoomIsOneSecond) {
    entt::registry registry;
    Timeline timeline(registry);
    TimelineWidget widget(&timeline);
    timeline.setFrameRate(30.0);

    EXPECT_EQ(widget.getZoomIndex(), TimelineWidget::DEFAULT_ZOOM_INDEX);
    EXPECT_STREQ(TimelineWidget::ZOOM_STOPS[widget.getZoomIndex()].label, "1s");
    EXPECT_EQ(widget.framesPerTick(), 30);
}

TEST(TimelineZoomLadder, ZoomIndexIsClamped) {
    entt::registry registry;
    Timeline timeline(registry);
    TimelineWidget widget(&timeline);

    widget.setZoomIndex(-5);
    EXPECT_EQ(widget.getZoomIndex(), 0);

    widget.setZoomIndex(TimelineWidget::ZOOM_LEVEL_COUNT + 10);
    EXPECT_EQ(widget.getZoomIndex(), TimelineWidget::ZOOM_LEVEL_COUNT - 1);
}
