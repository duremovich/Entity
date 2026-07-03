#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "entity/core/Types.hpp"

namespace entity::warmset {

/** One clip's timeline placement, extracted from the Clip component. */
struct ClipSpan {
    entt::entity entity{entt::null};
    FrameNumber  startFrame{0};
    FrameNumber  duration{0};
};

struct Params {
    FrameNumber playheadFrame{0};
    double      timelineFps{30.0};
    double      lookaheadSeconds{20.0};   // Settings::decodeLookaheadSeconds
    double      backwardSeconds{2.0};     // fixed per spec
    double      graceSeconds{5.0};        // fixed per spec
    int         cap{32};                  // Settings::decodeWorkerCap
    FrameNumber armedCueFrame{-1};        // -1 = no armed cue
    int64_t     nowNs{0};                 // caller-supplied steady_clock now
};

/**
 * Decide which clips deserve live decode workers this tick.
 *
 * warm = (overlaps [playhead - backward, playhead + lookahead])
 *      ∪ (overlaps [armedCueFrame, armedCueFrame + lookahead])   [if armed]
 *      capped to `cap` by ascending distance-to-playhead (armed-only
 *      clips use distance-to-armed-frame), then
 *      ∪ grace members (left the warm set < graceSeconds ago).
 *
 * Grace members ride on top of the cap: they are already-open decoders
 * being wound down, and re-capping them would re-churn the exact edge
 * the grace period exists to damp.
 *
 * `lastWarmNs` is caller-owned state: entries are stamped `nowNs` for every
 * clip in the returned set, and entries older than graceSeconds whose clip
 * is not warm are erased (bounds the map). Pure otherwise — no engine
 * dependencies, unit-testable.
 */
std::unordered_set<entt::entity> compute(
    const std::vector<ClipSpan>& clips,
    const Params& p,
    std::unordered_map<entt::entity, int64_t>& lastWarmNs);

} // namespace entity::warmset
