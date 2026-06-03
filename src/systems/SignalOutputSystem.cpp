// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)

#include "entity/systems/SignalOutputSystem.hpp"
#include "entity/components/AnimatedProperties.hpp"  // AnimatableProperty::SignalValue

#include <algorithm>
#include <cmath>

namespace entity {

// ---------------------------------------------------------------------------
// reset
// ---------------------------------------------------------------------------

void SignalOutputSystem::reset() {
    m_momentary.clear();
    m_continuous.clear();
}

// ---------------------------------------------------------------------------
// evaluate
// ---------------------------------------------------------------------------

void SignalOutputSystem::evaluate(
        const std::vector<bus::SignalLayerSnapshot>& signalLayers,
        FrameNumber prevFrame,
        FrameNumber curFrame,
        bool playing,
        bool atBreak,
        std::vector<bus::SignalEmit>& emitsOut) {

    for (const auto& sl : signalLayers) {
        const bool isMomentary = (sl.mode == 0); // SignalLayer::Mode::Momentary = 0

        if (isMomentary) {
            // --- Momentary crossing detector -----------------------------------
            // Re-arm rule: curFrame < startFrame means the playhead is before
            // this layer's start (rewind case). Always re-arm here so the next
            // forward crossing fires even after a seek back.
            auto& st = m_momentary[sl.entity];
            if (curFrame < sl.startFrame) {
                st.armed = true;
            }

            // Forward-crossing: playhead just passed startFrame on this tick.
            // crossing requires playing, !atBreak, forward motion across start.
            // atBreak suppresses without consuming the arm: the parked-at-break
            // state is not a "crossing"; the next real forward cross after GO fires.
            const bool crossed = playing
                              && !atBreak
                              && prevFrame < sl.startFrame
                              && curFrame  >= sl.startFrame;

            if (crossed && st.armed) {
                st.armed = false;
                // Momentary args are all literals (no value binding / range map).
                emitsOut.push_back(buildEmit(sl, 0.0f));
            }

        } else {
            // --- Continuous emitter --------------------------------------------
            // Skip conditions per the plan: !inSpan || !playing || atBreak.
            const FrameNumber layEnd = sl.startFrame + sl.duration;
            const bool inSpan = (curFrame >= sl.startFrame && curFrame < layEnd);
            if (!inSpan || !playing || atBreak) continue;

            // Rate-limit: emit at most once per distinct curFrame.
            auto& cst = m_continuous[sl.entity];
            if (cst.lastFrame == curFrame) continue;
            cst.lastFrame = curFrame;

            // Resolve float value from the chosen source.
            float rawValue = 0.0f;
            const bool isProgress = (sl.valueSource == 0); // ValueSource::Progress = 0

            if (isProgress) {
                if (sl.duration > 0) {
                    const float t = static_cast<float>(curFrame - sl.startFrame)
                                  / static_cast<float>(sl.duration);
                    rawValue = std::clamp(t, 0.0f, 1.0f);
                }
            } else {
                // Keyframe (ValueSource::Keyframe = 1).
                const FrameNumber localFrame = (curFrame >= sl.startFrame)
                    ? (curFrame - sl.startFrame)
                    : FrameNumber{0};
                rawValue = evaluateKeyframeTrack(sl, localFrame);
            }

            const float mapped = applyRangeMap(rawValue,
                                               sl.srcMin, sl.srcMax,
                                               sl.outMin, sl.outMax);
            emitsOut.push_back(buildEmit(sl, mapped));
        }
    }
}

// ---------------------------------------------------------------------------
// buildEmit
// ---------------------------------------------------------------------------

bus::SignalEmit SignalOutputSystem::buildEmit(const bus::SignalLayerSnapshot& sl,
                                               float boundValue) {
    bus::SignalEmit emit;
    emit.transport = bus::SignalEmit::Transport::OSC;
    emit.address   = sl.address;

    // Value-bound arg tiebreak: FIRST arg slot wins (valueBoundArgIndex, set
    // by the bake to the first isValueBound arg). Any arg at a later position
    // that the authoring layer accidentally marked bound is treated as a
    // literal — deterministic behavior when the invariant is violated.
    // This is a documented contract, unit-tested in SignalOutputSystemTest.
    for (std::size_t i = 0; i < sl.args.size(); ++i) {
        const auto& src = sl.args[i];
        bus::SignalArg dst;
        dst.type = src.type;
        dst.i    = src.i;
        dst.f    = src.f;
        dst.s    = src.s;

        if (static_cast<int>(i) == sl.valueBoundArgIndex) {
            switch (static_cast<bus::SignalArg::Type>(sl.outType)) {
                case bus::SignalArg::Type::Int:
                    dst.type = bus::SignalArg::Type::Int;
                    dst.i    = static_cast<std::int32_t>(boundValue); // truncate per spec
                    break;
                case bus::SignalArg::Type::String:
                    dst.type = bus::SignalArg::Type::String;
                    dst.s    = std::to_string(boundValue);
                    break;
                case bus::SignalArg::Type::Float:
                default:
                    dst.type = bus::SignalArg::Type::Float;
                    dst.f    = boundValue;
                    break;
            }
        }

        emit.args.push_back(dst);
    }

    return emit;
}

// ---------------------------------------------------------------------------
// evaluateKeyframeTrack  (linear interpolation only, v1)
// ---------------------------------------------------------------------------

float SignalOutputSystem::evaluateKeyframeTrack(
        const bus::SignalLayerSnapshot& sl, FrameNumber localFrame) {
    const int signalValueProp = static_cast<int>(AnimatableProperty::SignalValue);

    for (const auto& track : sl.tracks) {
        if (track.property != signalValueProp) continue;
        if (!track.enabled || track.keyframes.empty()) return 0.0f;

        const auto& kfs = track.keyframes;
        if (localFrame <= kfs.front().frame) return kfs.front().value;
        if (localFrame >= kfs.back().frame)  return kfs.back().value;

        // Phase 5 v1: linear interpolation only.
        // Honoring the per-keyframe interpolation enum is an explicit follow-on.
        auto it = std::lower_bound(kfs.begin(), kfs.end(), localFrame,
            [](const bus::BakedKeyframe& k, FrameNumber f) {
                return k.frame < f;
            });

        if (it == kfs.end())            return kfs.back().value;
        if (it->frame == localFrame)    return it->value;
        if (it == kfs.begin())          return kfs.front().value;

        const auto& prev = *(it - 1);
        const auto& next = *it;
        const float t = static_cast<float>(localFrame - prev.frame)
                      / static_cast<float>(next.frame - prev.frame);
        return prev.value + (next.value - prev.value) * t;
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// applyRangeMap
// ---------------------------------------------------------------------------

float SignalOutputSystem::applyRangeMap(float value,
                                         float srcMin, float srcMax,
                                         float outMin, float outMax) {
    if (srcMax == srcMin) return outMin;
    const float t      = (value - srcMin) / (srcMax - srcMin);
    return outMin + std::clamp(t, 0.0f, 1.0f) * (outMax - outMin);
}

} // namespace entity
