// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#pragma once

#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace entity {

/**
 * SignalOutputSystem -- evaluates active SignalLayerSnapshots and produces
 * bus::SignalEmit events each tick.
 *
 * Threading: show-affinity (Phase 6). Pure logic, no registry writes.
 * All state is owned in this system instance, never in components
 * (ADR-0014 compliant).
 *
 * Momentary crossing:
 *   armed = true initially. Re-arms when curFrame < startFrame (rewind).
 *   crossed = playing && !atBreak && prevFrame < startFrame && curFrame >= startFrame.
 *   Fire once when (crossed && armed); then disarm until re-armed.
 *   This is a forward-only, playing-gated detector:
 *     - scrub (playing=false) never fires.
 *     - backward motion (prevFrame > curFrame across start) never fires.
 *     - atBreak suppresses without consuming the arm (next forward cross fires).
 *
 * Continuous:
 *   Active while curFrame in [startFrame, startFrame+duration) AND playing AND !atBreak.
 *   Rate-limited: one emit per distinct curFrame (m_lastContinuousFrame per entity).
 *   Value sourced from Progress (normalized position) or Keyframe (linear BakedTrack).
 *   Range-mapped then cast to outType; inserted at valueBoundArgIndex slot.
 *
 * Value-bound arg tiebreak:
 *   At most one arg should be value-bound (authoring invariant). The bake site
 *   already sets valueBoundArgIndex to the FIRST isValueBound arg. If violated,
 *   the system uses only that index; all other arg slots pass through as literals.
 *   Documented + unit-tested. Never crashes on multiple bound args.
 *
 * reset() clears all per-entity armed and continuous state. Call on project load
 * or seek.
 */
class SignalOutputSystem {
public:
    SignalOutputSystem() = default;

    /**
     * Evaluate one show-thread tick.
     *
     * @param signalLayers   SceneSnapshot::signalLayers for this tick.
     * @param prevFrame      Timeline frame from the PREVIOUS tick (show-thread local).
     * @param curFrame       Timeline frame for THIS tick.
     * @param playing        True when playbackState == Playing.
     * @param atBreak        True when parked at a section break.
     * @param emitsOut       Output: appended with one SignalEmit per firing layer.
     */
    void evaluate(const std::vector<bus::SignalLayerSnapshot>& signalLayers,
                  FrameNumber prevFrame,
                  FrameNumber curFrame,
                  bool playing,
                  bool atBreak,
                  std::vector<bus::SignalEmit>& emitsOut);

    /**
     * Reset all per-entity state. Call on project load, seek, or stop.
     */
    void reset();

private:
    // Per-entity momentary state.
    struct MomentaryState {
        bool armed{true};   // true = will fire on next qualifying crossing
    };

    // Per-entity last-emitted frame for continuous rate limiting.
    struct ContinuousState {
        FrameNumber lastFrame{-1};  // -1 = never emitted
    };

    // Keyed by entity uint64_t (== entt::entity cast).
    std::unordered_map<std::uint64_t, MomentaryState>  m_momentary;
    std::unordered_map<std::uint64_t, ContinuousState> m_continuous;

    // Build a SignalEmit from a snapshot layer, substituting `boundValue`
    // into the arg at valueBoundArgIndex (ignored when index < 0).
    static bus::SignalEmit buildEmit(const bus::SignalLayerSnapshot& sl,
                                     float boundValue);

    // Evaluate the first SignalValue BakedTrack at layer-local frame.
    // Linear interpolation only (v1); returns 0.0f when no track found.
    static float evaluateKeyframeTrack(const bus::SignalLayerSnapshot& sl,
                                        FrameNumber localFrame);

    // Linear range map: (value - srcMin)/(srcMax - srcMin) -> [outMin, outMax].
    // Clamps result; guards against degenerate srcMax==srcMin.
    static float applyRangeMap(float value, float srcMin, float srcMax,
                                float outMin, float outMax);
};

} // namespace entity
