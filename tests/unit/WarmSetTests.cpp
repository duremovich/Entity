// warmset::compute() unit tests — the pure decode-worker budget helper.
// Covers window membership, armed-cue union, cap-by-distance, and grace
// hysteresis (spec: entity-decode-worker-budget).

#include "entity/systems/WarmSet.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace entity;
using warmset::ClipSpan;
using warmset::Params;

namespace {

constexpr int64_t kSecNs = 1'000'000'000LL;

entt::entity ent(uint32_t i) { return static_cast<entt::entity>(i); }

Params baseParams() {
    Params p;
    p.playheadFrame = 1000;
    p.timelineFps = 30.0;
    p.lookaheadSeconds = 20.0;   // 600 frames
    p.backwardSeconds = 2.0;     // 60 frames
    p.graceSeconds = 5.0;
    p.cap = 32;
    p.nowNs = 100 * kSecNs;
    return p;
}

} // namespace

// 1. Window membership: active, imminent, just-passed, far-future, far-past.
TEST(WarmSet, WindowMembership) {
    std::unordered_map<entt::entity, int64_t> lastWarm;
    std::vector<ClipSpan> clips = {
        {ent(1),  900, 200},   // active under playhead → warm
        {ent(2), 1500, 100},   // starts inside +600 lookahead → warm
        {ent(3),  950,  20},   // ended 30f ago, inside -60 backward → warm
        {ent(4), 1601, 100},   // starts 1f past window end → cold
        {ent(5),  100, 200},   // long past → cold
    };
    auto warm = warmset::compute(clips, baseParams(), lastWarm);
    EXPECT_TRUE(warm.contains(ent(1)));
    EXPECT_TRUE(warm.contains(ent(2)));
    EXPECT_TRUE(warm.contains(ent(3)));
    EXPECT_FALSE(warm.contains(ent(4)));
    EXPECT_FALSE(warm.contains(ent(5)));
}

// 2. Armed cue window unions in distant clips.
TEST(WarmSet, ArmedCue) {
    std::unordered_map<entt::entity, int64_t> lastWarm;
    std::vector<ClipSpan> clips = {
        {ent(1), 1000, 50},    // at playhead
        {ent(2), 9000, 100},   // at armed cue → warm only because armed
        {ent(3), 9700, 100},   // past armed window end (9000+600) → cold
    };
    auto p = baseParams();
    p.armedCueFrame = 9000;
    auto warm = warmset::compute(clips, p, lastWarm);
    EXPECT_TRUE(warm.contains(ent(1)));
    EXPECT_TRUE(warm.contains(ent(2)));
    EXPECT_FALSE(warm.contains(ent(3)));
}

// 3. Cap keeps nearest; armed clips count within cap.
TEST(WarmSet, CapNearestWins) {
    std::unordered_map<entt::entity, int64_t> lastWarm;
    std::vector<ClipSpan> clips;
    for (uint32_t i = 0; i < 10; ++i)
        clips.push_back({ent(i), 1000 + FrameNumber(i) * 50, 40}); // increasing distance
    auto p = baseParams();
    p.cap = 4;
    auto warm = warmset::compute(clips, p, lastWarm);
    EXPECT_EQ(warm.size(), 4u);
    EXPECT_TRUE(warm.contains(ent(0)));
    EXPECT_TRUE(warm.contains(ent(1)));
    EXPECT_TRUE(warm.contains(ent(2)));
    EXPECT_TRUE(warm.contains(ent(3)));
    EXPECT_FALSE(warm.contains(ent(9)));
}

// 4. Grace: a clip that was warm stays warm < 5 s after leaving the window,
//    drops after; grace members don't consume cap.
TEST(WarmSet, GraceHysteresis) {
    std::unordered_map<entt::entity, int64_t> lastWarm;
    std::vector<ClipSpan> clips = { {ent(1), 1000, 50} };
    auto p = baseParams();
    warmset::compute(clips, p, lastWarm);          // stamp warm at t=100s

    p.playheadFrame = 5000;                        // jump far away
    p.nowNs = 103 * kSecNs;                        // +3 s < grace
    auto warm2 = warmset::compute(clips, p, lastWarm);
    EXPECT_TRUE(warm2.contains(ent(1)));           // grace holds it

    p.nowNs = 106 * kSecNs;                        // +6 s > grace
    auto warm3 = warmset::compute(clips, p, lastWarm);
    EXPECT_FALSE(warm3.contains(ent(1)));
    EXPECT_FALSE(lastWarm.contains(ent(1)));       // stale entry pruned
}
