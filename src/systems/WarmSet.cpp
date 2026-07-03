#include "entity/systems/WarmSet.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace entity::warmset {

namespace {

// Distance from a clip's [start, end) interval to a frame; 0 when inside.
int64_t intervalDistance(const ClipSpan& c, FrameNumber frame) {
    if (frame < c.startFrame) return int64_t(c.startFrame) - int64_t(frame);
    const FrameNumber end = c.startFrame + c.duration;
    if (frame >= end) return int64_t(frame) - int64_t(end) + 1;
    return 0;
}

bool overlaps(const ClipSpan& c, FrameNumber winStart, FrameNumber winEnd) {
    // [startFrame, startFrame+duration) vs [winStart, winEnd] inclusive end
    return c.startFrame <= winEnd && (c.startFrame + c.duration) > winStart;
}

} // namespace

std::unordered_set<entt::entity> compute(
    const std::vector<ClipSpan>& clips,
    const Params& p,
    std::unordered_map<entt::entity, int64_t>& lastWarmNs) {

    const double fps = std::max(1.0, p.timelineFps);
    const auto back  = static_cast<FrameNumber>(std::llround(p.backwardSeconds  * fps));
    const auto ahead = static_cast<FrameNumber>(std::llround(p.lookaheadSeconds * fps));

    const FrameNumber winStart = (p.playheadFrame > back) ? p.playheadFrame - back : 0;
    const FrameNumber winEnd   = p.playheadFrame + ahead;
    const bool armed           = (p.armedCueFrame >= 0);
    const FrameNumber armEnd   = armed ? p.armedCueFrame + ahead : 0;

    // Candidates = window ∪ armed window, keyed by best distance for cap sort.
    struct Cand { entt::entity e; int64_t dist; };
    std::vector<Cand> cands;
    cands.reserve(clips.size());
    for (const ClipSpan& c : clips) {
        const bool inWin = overlaps(c, winStart, winEnd);
        const bool inArm = armed && overlaps(c, p.armedCueFrame, armEnd);
        if (!inWin && !inArm) continue;
        int64_t d = inWin ? intervalDistance(c, p.playheadFrame)
                          : std::numeric_limits<int64_t>::max();
        if (inArm) d = std::min(d, intervalDistance(c, p.armedCueFrame));
        cands.push_back({c.entity, d});
    }

    if (static_cast<int>(cands.size()) > p.cap) {
        std::nth_element(cands.begin(), cands.begin() + p.cap, cands.end(),
                         [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
        cands.resize(static_cast<size_t>(p.cap));
    }

    std::unordered_set<entt::entity> warm;
    warm.reserve(cands.size());
    for (const Cand& c : cands) {
        warm.insert(c.e);
        lastWarmNs[c.e] = p.nowNs;
    }

    // Grace: recently-warm clips stay warm (uncapped — they're open decoders
    // winding down). Prune expired entries for clips no longer warm.
    const auto graceNs = static_cast<int64_t>(p.graceSeconds * 1e9);
    for (auto it = lastWarmNs.begin(); it != lastWarmNs.end();) {
        if (warm.contains(it->first)) { ++it; continue; }
        if (p.nowNs - it->second <= graceNs) {
            warm.insert(it->first);
            ++it;
        } else {
            it = lastWarmNs.erase(it);
        }
    }
    return warm;
}

} // namespace entity::warmset
