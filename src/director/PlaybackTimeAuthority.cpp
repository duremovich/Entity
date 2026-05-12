#include "entity/director/PlaybackTimeAuthority.hpp"

#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectChainRenderTargets.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/MunchersGameState.hpp"
#include "entity/components/MappingSurface.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/timeline/Timeline.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <utility>

namespace entity {

namespace {

constexpr TransportState toTransportState(PlaybackState s) {
    switch (s) {
        case PlaybackState::Stopped: return TransportState::Stopped;
        case PlaybackState::Playing: return TransportState::Playing;
        case PlaybackState::Paused:  return TransportState::Paused;
    }
    return TransportState::Stopped;
}

// entt::null casts to a large value; normalise to 0 so the bus wire
// representation is stable regardless of EnTT's internal sentinel.
constexpr std::uint64_t entityToU64(entt::entity e) {
    return (e == entt::null) ? 0ull : static_cast<std::uint64_t>(e);
}

// Reconstruct a Clip value from a ClipCatalogEntry for use with the pure
// mapToMediaFrame(const Clip&, FrameNumber) overload and computeSectionFadeMultiplier.
// FFmpeg pointer fields stay null — only the scheduling/math fields matter here.
Clip clipFromCatalog(const bus::ClipCatalogEntry& e) {
    Clip c;
    c.startFrame      = e.startFrame;
    c.duration        = e.duration;
    c.mediaStartFrame = e.mediaStartFrame;
    c.mediaOutFrame   = e.mediaOutFrame;
    c.framerate       = e.framerate;
    c.playbackMode    = static_cast<PlaybackMode>(e.playbackMode);
    c.sectionBehavior = static_cast<SectionBehavior>(e.sectionBehavior);
    // targetScreen: convert from uint64 back to entt::entity
    c.targetScreen = (e.targetScreen == UINT64_MAX)
        ? entt::null
        : static_cast<entt::entity>(static_cast<std::uint32_t>(e.targetScreen));
    return c;
}

// Show-side mapToMediaFrame: mirrors the entity-aware 3-arg overload but reads
// phase data from the ClipCatalogEntry instead of the registry.
FrameNumber mapToMediaFrameFromCatalog(const bus::ClipCatalogEntry& e,
                                       FrameNumber timelineFrame,
                                       double timelineFrameRate) {
    const Clip clip = clipFromCatalog(e);
    const FrameNumber clipEnd = clip.startFrame + clip.duration;
    const FrameNumber sourceLength = effectivePlaybackLength(clip);

    // tail short-circuit (same as entity-aware overload)
    if (timelineFrame >= clipEnd && e.hasPhase && e.phase_tailHoldMediaFrame >= 0) {
        return e.phase_tailHoldMediaFrame;
    }

    const bool inContinuation = e.hasPhase && e.phase_inContinuation
        && clip.sectionBehavior == SectionBehavior::Normal;

    if (!inContinuation) {
        // post-break anchor path
        if (e.hasPhase && e.phase_postBreakMediaAnchor >= 0 && timelineFrame < clipEnd) {
            const double frameRateRatio = (timelineFrameRate > 0.0)
                ? clip.framerate / timelineFrameRate : 1.0;
            if (sourceLength <= 0) return clip.mediaStartFrame;

            const double timelineDelta =
                static_cast<double>(timelineFrame - e.phase_anchorTimelineFrame);
            const double localFloat =
                static_cast<double>(e.phase_postBreakMediaAnchor - clip.mediaStartFrame)
                + timelineDelta * frameRateRatio;
            const FrameNumber sourceLocalFrame = static_cast<FrameNumber>(
                std::floor(std::max(localFloat, 0.0)));

            if (sourceLocalFrame < sourceLength) {
                return clip.mediaStartFrame + sourceLocalFrame;
            }
            switch (clip.playbackMode) {
                case PlaybackMode::Freeze:
                    return clip.mediaStartFrame + sourceLength - 1;
                case PlaybackMode::Loop:
                    return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
                case PlaybackMode::PingPong: {
                    const FrameNumber cycle = sourceLocalFrame / sourceLength;
                    const FrameNumber pos   = sourceLocalFrame % sourceLength;
                    return (cycle % 2 == 0)
                        ? clip.mediaStartFrame + pos
                        : clip.mediaStartFrame + (sourceLength - 1 - pos);
                }
            }
            return clip.mediaStartFrame + sourceLength - 1;
        }

        // natural timeline-derived path (2-arg equivalent, inlined)
        const double frameRateRatio = (timelineFrameRate > 0.0)
            ? clip.framerate / timelineFrameRate : 1.0;
        if (timelineFrame >= clipEnd) {
            return clip.mediaStartFrame + std::max<FrameNumber>(0, sourceLength - 1);
        }
        const FrameNumber localFrame = timelineFrame - clip.startFrame;
        const FrameNumber sourceLocalFrame = static_cast<FrameNumber>(
            std::floor(localFrame * frameRateRatio));
        if (sourceLocalFrame < sourceLength) {
            return clip.mediaStartFrame + sourceLocalFrame;
        }
        switch (clip.playbackMode) {
            case PlaybackMode::Freeze:
                return clip.mediaStartFrame + sourceLength - 1;
            case PlaybackMode::Loop:
                return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
            case PlaybackMode::PingPong: {
                const FrameNumber cycle = sourceLocalFrame / sourceLength;
                const FrameNumber pos   = sourceLocalFrame % sourceLength;
                return (cycle % 2 == 0)
                    ? clip.mediaStartFrame + pos
                    : clip.mediaStartFrame + (sourceLength - 1 - pos);
            }
        }
        return clip.mediaStartFrame + sourceLength - 1;
    }

    // continuation path: derive from accumulated source phase
    if (sourceLength <= 0) return clip.mediaStartFrame;
    const double phaseClamped = std::max(e.phase_sourcePhaseFrames, 0.0);
    const FrameNumber phaseFrame = static_cast<FrameNumber>(std::floor(phaseClamped));
    switch (clip.playbackMode) {
        case PlaybackMode::Freeze:
            return clip.mediaStartFrame + std::min(phaseFrame, sourceLength - 1);
        case PlaybackMode::Loop:
            return clip.mediaStartFrame + (phaseFrame % sourceLength);
        case PlaybackMode::PingPong: {
            const FrameNumber cycle = phaseFrame / sourceLength;
            const FrameNumber pos   = phaseFrame % sourceLength;
            return (cycle % 2 == 0)
                ? clip.mediaStartFrame + pos
                : clip.mediaStartFrame + (sourceLength - 1 - pos);
        }
    }
    return clip.mediaStartFrame + sourceLength - 1;
}

// -- Show-thread animation evaluator (NEW-07) ------------------------------
//
// The show thread evaluates `bus::BakedTrack`s out of the SceneSnapshot at
// each render frame so animation stays alive while the editor thread is
// stalled (and therefore not re-baking new snapshots / re-ticking
// AnimationSystem). Math mirrors entity::KeyframeTrack::evaluate in
// include/entity/components/AnimatedProperties.hpp — keep them in sync.

// Mirror of KeyframeTrack::interpolate but operating on entity::Keyframe
// (in-memory format). Used by the effect-bake path; the matching show-side
// re-evaluator works on bus::BakedKeyframe via interpolateBakedKeyframes
// below — same math, two forms of the input.
float interpolateInMemoryKeyframes(const Keyframe& a, const Keyframe& b, float t) {
    if (a.interpolation == InterpolationType::Step) return a.value;
    const float delta = b.value - a.value;
    switch (b.interpolation) {
        case InterpolationType::Step:      return a.value + delta * t;
        case InterpolationType::EaseIn:    return a.value + delta * (t * t);
        case InterpolationType::EaseOut: {
            const float inv = 1.0f - t;
            return a.value + delta * (1.0f - inv * inv);
        }
        case InterpolationType::EaseInOut: {
            const float mt  = 1.0f - t;
            const float t2  = t * t;
            const float t3  = t2 * t;
            const float y   = 3.0f * mt * t2 + t3;
            return a.value + delta * y;
        }
        case InterpolationType::Linear:
        default:                           return a.value + delta * t;
    }
}

float evaluateInMemoryKeyframes(const std::vector<Keyframe>& kfs,
                                  FrameNumber frame,
                                  float defaultIfEmpty) {
    if (kfs.empty()) return defaultIfEmpty;
    if (frame <= kfs.front().frame) return kfs.front().value;
    if (frame >= kfs.back().frame)  return kfs.back().value;
    auto it = std::lower_bound(kfs.begin(), kfs.end(), frame,
        [](const Keyframe& k, FrameNumber f) { return k.frame < f; });
    if (it != kfs.end() && it->frame == frame) return it->value;
    if (it == kfs.begin() || it == kfs.end()) return kfs.back().value;
    const auto& prev = *(it - 1);
    const auto& next = *it;
    const float t = static_cast<float>(frame - prev.frame)
                  / static_cast<float>(next.frame - prev.frame);
    return interpolateInMemoryKeyframes(prev, next, t);
}

// Marshal one ParamValue into its 16-byte cbuffer slot. Slot layout per
// shaders/effects/_effect_common.hlsli: Float→.x, Vec2→.xy, Vec3→.xyz,
// Color→.xyzw, Int/Enum→asint(.x), Bool→(.x!=0).
void marshalParamValueToSlot(const ParamValue& v, std::uint8_t* slotPtr) {
    // Zero the slot first so unused components read as 0.0.
    std::memset(slotPtr, 0, 16);
    switch (v.type) {
        case ParamValue::Type::Float:
            std::memcpy(slotPtr, &v.f4[0], sizeof(float));
            break;
        case ParamValue::Type::Vec2:
            std::memcpy(slotPtr, v.f4, sizeof(float) * 2);
            break;
        case ParamValue::Type::Vec3:
            std::memcpy(slotPtr, v.f4, sizeof(float) * 3);
            break;
        case ParamValue::Type::Color:
            std::memcpy(slotPtr, v.f4, sizeof(float) * 4);
            break;
        case ParamValue::Type::Int:
        case ParamValue::Type::Enum: {
            const float asF = std::bit_cast<float>(v.i);
            std::memcpy(slotPtr, &asF, sizeof(float));
            break;
        }
        case ParamValue::Type::Bool: {
            const float asF = v.b ? 1.0f : 0.0f;
            std::memcpy(slotPtr, &asF, sizeof(float));
            break;
        }
    }
}

float interpolateBakedKeyframes(const bus::BakedKeyframe& a,
                                const bus::BakedKeyframe& b,
                                float t) {
    // Step on `a` holds the start value across the interval.
    if (static_cast<InterpolationType>(a.interpolation) == InterpolationType::Step) {
        return a.value;
    }
    const float delta = b.value - a.value;
    switch (static_cast<InterpolationType>(b.interpolation)) {
        case InterpolationType::Step:      return a.value + delta * t;
        case InterpolationType::EaseIn:    return a.value + delta * (t * t);
        case InterpolationType::EaseOut: {
            const float inv = 1.0f - t;
            return a.value + delta * (1.0f - inv * inv);
        }
        case InterpolationType::EaseInOut: {
            // Cubic bezier with P0=(0,0), P1=(easeIn,0), P2=(easeOut,1), P3=(1,1).
            // Matches the simplified bezier in KeyframeTrack::cubicBezier
            // — same omissions (P1.y = 0 zeroes its term).
            const float mt  = 1.0f - t;
            const float t2  = t * t;
            const float t3  = t2 * t;
            const float mt2 = mt * mt;
            const float y   = 3.0f * mt  * t2 * 1.0f  // P2.y term
                            + t3        * 1.0f;       // P3.y term
            return a.value + delta * y;
        }
        case InterpolationType::Linear:
        default:                           return a.value + delta * t;
    }
}

float defaultValueForProperty(AnimatableProperty p) {
    switch (p) {
        case AnimatableProperty::ScaleX:
        case AnimatableProperty::ScaleY:
        case AnimatableProperty::ScaleZ:   // identity scale — 0 would collapse a screen to flat
        case AnimatableProperty::Opacity:  return 1.0f;
        case AnimatableProperty::PositionX:
        case AnimatableProperty::PositionY:
        case AnimatableProperty::PositionZ:
        case AnimatableProperty::Rotation:
        case AnimatableProperty::RotationX:
        case AnimatableProperty::RotationY:
        default:                            return 0.0f;
    }
}

float evaluateBakedTrack(const bus::BakedTrack& track, FrameNumber frame) {
    const auto property = static_cast<AnimatableProperty>(track.property);
    if (track.keyframes.empty()) return defaultValueForProperty(property);

    const auto& first = track.keyframes.front();
    const auto& last  = track.keyframes.back();
    if (frame <= first.frame) return first.value;
    if (frame >= last.frame)  return last.value;

    // First keyframe with .frame >= frame; binary search.
    auto it = std::lower_bound(
        track.keyframes.begin(), track.keyframes.end(), frame,
        [](const bus::BakedKeyframe& k, FrameNumber f) { return k.frame < f; });

    if (it != track.keyframes.end() && it->frame == frame) return it->value;
    if (it == track.keyframes.begin() || it == track.keyframes.end()) {
        return last.value;
    }

    const auto& prev = *(it - 1);
    const auto& next = *it;
    const float t = static_cast<float>(frame - prev.frame)
                  / static_cast<float>(next.frame - prev.frame);
    return interpolateBakedKeyframes(prev, next, t);
}

// Apply animation tracks from the catalog to the render state. Touches
// only the snapshot inputs + output ClipRenderState — no registry reads.
// Called from the show thread.
void applyBakedAnimation(const bus::ClipCatalogEntry& ce,
                         FrameNumber currentFrame,
                         bus::ClipRenderState& out) {
    if (ce.tracks.empty()) return;  // static clip: catalog matrix/opacity are authoritative

    // localFrame mirrors AnimationSystem::update: clip-local frame, clamped
    // to non-negative. If the clip isn't active at currentFrame we still
    // get sensible values (clamped to first/last keyframe).
    const FrameNumber localFrame =
        (currentFrame >= ce.startFrame) ? (currentFrame - ce.startFrame) : FrameNumber{0};

    glm::vec3 position(ce.position[0], ce.position[1], ce.position[2]);
    glm::vec3 rotation(ce.rotation[0], ce.rotation[1], ce.rotation[2]);
    glm::vec3 scale(ce.scale[0], ce.scale[1], ce.scale[2]);
    float     opacity = ce.opacity;
    bool      transformDirty = false;

    for (const auto& track : ce.tracks) {
        if (!track.enabled || track.keyframes.empty()) continue;
        const float v = evaluateBakedTrack(track, localFrame);
        switch (static_cast<AnimatableProperty>(track.property)) {
            case AnimatableProperty::PositionX: position.x = v; transformDirty = true; break;
            case AnimatableProperty::PositionY: position.y = v; transformDirty = true; break;
            case AnimatableProperty::Rotation:  rotation.z = v; transformDirty = true; break;
            case AnimatableProperty::ScaleX:    scale.x    = v; transformDirty = true; break;
            case AnimatableProperty::ScaleY:    scale.y    = v; transformDirty = true; break;
            case AnimatableProperty::Opacity:   opacity    = v;                         break;
        }
    }

    if (transformDirty) {
        // Mirrors Transform::updateMatrix: Translation * Rot(Z*Y*X) * Scale.
        glm::mat4 m(1.0f);
        m = glm::translate(m, position);
        m = glm::rotate(m, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        m = glm::rotate(m, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        m = glm::rotate(m, glm::radians(rotation.x), glm::vec3(1, 0, 0));
        m = glm::scale(m, scale);
        std::memcpy(out.transformMatrix.data(), glm::value_ptr(m),
                    sizeof(out.transformMatrix));
    }
    out.opacity = opacity;
}

} // namespace

PlaybackTimeAuthority::PlaybackTimeAuthority(entt::registry& registry, Timeline* timeline)
    : m_registry(registry)
    , m_timeline(timeline)
{}

PlaybackTimeAuthority::~PlaybackTimeAuthority() = default;

void PlaybackTimeAuthority::startTiming() {
    m_startTime = Clock::now();
    m_lastFrameTime = m_startTime;
    m_deltaTime = 0.0;
    m_elapsedTime = 0.0;
    m_frameCount = 0;
}

void PlaybackTimeAuthority::updateTiming() {
    TimePoint currentTime = Clock::now();
    std::chrono::duration<double> delta = currentTime - m_lastFrameTime;
    m_deltaTime = delta.count();
    std::chrono::duration<double> elapsed = currentTime - m_startTime;
    m_elapsedTime = elapsed.count();
    m_lastFrameTime = currentTime;
}

FrameNumber PlaybackTimeAuthority::sectionFadeTailFrames(FrameNumber endFrame) const {
    if (!m_timeline) return 0;
    const auto& sections = m_timeline->getSections();
    if (sections.empty()) return 0;
    const double timelineFrameRate = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;
    constexpr FrameNumber snapTol = 1;
    auto absDiff = [](FrameNumber a, FrameNumber b) -> FrameNumber {
        return a >= b ? a - b : b - a;
    };
    for (const auto& sec : sections) {
        if (sec.fadeSeconds <= 0.0) continue;
        const FrameNumber breakFrame = static_cast<FrameNumber>(
            (static_cast<double>(sec.breakFrame) * timelineFrameRate) / 1000000.0);
        if (absDiff(breakFrame, endFrame) <= snapTol) {
            return static_cast<FrameNumber>(std::ceil(sec.fadeSeconds * timelineFrameRate));
        }
    }
    return 0;
}

bool PlaybackTimeAuthority::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    if (frame < clip.startFrame) return false;
    const FrameNumber endFrame = clip.startFrame + clip.duration;
    if (frame < endFrame) return true;
    // Phase 6 — extend the active set past clipEnd by the section's
    // fadeSeconds when the clip's end aligns with a break. This keeps
    // the clip visible (held last frame) while the post-break fade-out
    // ramps to zero.
    const FrameNumber tailFrames = sectionFadeTailFrames(endFrame);
    return frame < endFrame + tailFrames;
}

FrameNumber PlaybackTimeAuthority::mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    double frameRateRatio = clip.framerate / timelineFrameRate;

    FrameNumber sourceLength = effectivePlaybackLength(clip);

    // Phase 6 — post-end tail (held last decoded frame). isClipActiveAtFrame
    // gates entry into this window, so when timelineFrame >= clipEnd we
    // know we're inside the fade tail. Don't advance past the source.
    const FrameNumber clipEnd = clip.startFrame + clip.duration;
    if (timelineFrame >= clipEnd) {
        return clip.mediaStartFrame + std::max<FrameNumber>(0, sourceLength - 1);
    }

    FrameNumber localFrame = timelineFrame - clip.startFrame;
    FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

    if (sourceLocalFrame < sourceLength) {
        return clip.mediaStartFrame + sourceLocalFrame;
    }

    switch (clip.playbackMode) {
        case PlaybackMode::Freeze:
            return clip.mediaStartFrame + sourceLength - 1;
        case PlaybackMode::Loop:
            return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
        case PlaybackMode::PingPong: {
            FrameNumber cycle = sourceLocalFrame / sourceLength;
            FrameNumber pos = sourceLocalFrame % sourceLength;
            if (cycle % 2 == 0) {
                return clip.mediaStartFrame + pos;
            } else {
                return clip.mediaStartFrame + (sourceLength - 1 - pos);
            }
        }
    }
    return clip.mediaStartFrame + sourceLength - 1;
}

FrameNumber PlaybackTimeAuthority::mapToMediaFrame(entt::entity entity,
                                                   const Clip& clip,
                                                   FrameNumber timelineFrame) const {
    const FrameNumber clipEnd = clip.startFrame + clip.duration;

    // Continuation-phase override only applies when the component exists
    // AND the scheduler has placed the clip in continuation AND the clip's
    // current section behavior is still Normal. The behavior dropdown is
    // live UI — flipping a continuing Normal clip to Locked while paused
    // at a break must freeze it, not keep cycling. Falling through to the
    // timeline-derived path while paused does that, since timelineFrame
    // is frozen at the break frame.
    const ClipPlaybackPhase* phase = m_registry.try_get<ClipPlaybackPhase>(entity);

    // Phase 6 — post-end tail short-circuit. The clip's end has passed and
    // we're inside the section-fade tail window. Prefer the snapshot
    // tailHoldMediaFrame stashed by SectionScheduler::go() at the moment
    // continuation cleared (so a clip that was looping at the break holds
    // the frame the user actually saw); otherwise fall through to the
    // 2-arg overload's last-decoded-frame clamp.
    if (timelineFrame >= clipEnd && phase && phase->tailHoldMediaFrame >= 0) {
        return phase->tailHoldMediaFrame;
    }

    if (!phase || !phase->inContinuation
            || clip.sectionBehavior != SectionBehavior::Normal) {
        // Round-2 fixup, Phase 4 — post-break anchor. Spanning Normal-mode
        // clips that observed an at-break pause have a source-frame
        // anchor stamped by SectionScheduler::go(). The anchor branch
        // fires only after continuation has ended (post-GO,
        // !inContinuation) and only inside the clip's authored range
        // (timelineFrame < clipEnd; the tail-hold short-circuit above
        // already handled timelineFrame >= clipEnd). It maps mediaFrame
        // as `anchor + (timelineFrame - anchorTimelineFrame) * ratio`,
        // wrapped per playbackMode — same math as the natural-mapping
        // path but anchored to where the user was watching at GO.
        if (phase && phase->postBreakMediaAnchor >= 0
                && timelineFrame < clipEnd) {
            const double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
            const double frameRateRatio = (timelineFrameRate > 0.0)
                ? clip.framerate / timelineFrameRate : 1.0;
            const FrameNumber sourceLength = effectivePlaybackLength(clip);
            if (sourceLength <= 0) return clip.mediaStartFrame;

            const double timelineDelta =
                static_cast<double>(timelineFrame - phase->anchorTimelineFrame);
            const double localFloat =
                static_cast<double>(phase->postBreakMediaAnchor - clip.mediaStartFrame)
                + timelineDelta * frameRateRatio;
            const FrameNumber sourceLocalFrame = static_cast<FrameNumber>(
                std::floor(std::max(localFloat, 0.0)));

            if (sourceLocalFrame < sourceLength) {
                return clip.mediaStartFrame + sourceLocalFrame;
            }
            switch (clip.playbackMode) {
                case PlaybackMode::Freeze:
                    return clip.mediaStartFrame + sourceLength - 1;
                case PlaybackMode::Loop:
                    return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
                case PlaybackMode::PingPong: {
                    const FrameNumber cycle = sourceLocalFrame / sourceLength;
                    const FrameNumber pos   = sourceLocalFrame % sourceLength;
                    return (cycle % 2 == 0)
                        ? clip.mediaStartFrame + pos
                        : clip.mediaStartFrame + (sourceLength - 1 - pos);
                }
            }
            return clip.mediaStartFrame + sourceLength - 1;
        }
        return mapToMediaFrame(clip, timelineFrame);
    }

    // Continuation path: derive the media frame from the accumulated source
    // phase directly. Mirrors the Freeze/Loop/PingPong branches of the
    // timeline-derived path, but operates on `sourcePhaseFrames` instead
    // of the timeline-delta * frame-rate-ratio.
    const FrameNumber sourceLength = effectivePlaybackLength(clip);

    if (sourceLength <= 0) return clip.mediaStartFrame;

    const double phaseClamped = std::max(phase->sourcePhaseFrames, 0.0);
    const FrameNumber phaseFrame = static_cast<FrameNumber>(std::floor(phaseClamped));

    switch (clip.playbackMode) {
        case PlaybackMode::Freeze:
            return clip.mediaStartFrame + std::min(phaseFrame, sourceLength - 1);
        case PlaybackMode::Loop:
            return clip.mediaStartFrame + (phaseFrame % sourceLength);
        case PlaybackMode::PingPong: {
            const FrameNumber cycle = phaseFrame / sourceLength;
            const FrameNumber pos   = phaseFrame % sourceLength;
            if (cycle % 2 == 0) {
                return clip.mediaStartFrame + pos;
            } else {
                return clip.mediaStartFrame + (sourceLength - 1 - pos);
            }
        }
    }
    return clip.mediaStartFrame + sourceLength - 1;
}

std::string PlaybackTimeAuthority::lookupInputColorSpaceOverride(const Clip& clip) const {
    if (!m_projectManager) return {};
    if (clip.filepath.empty()) return {};
    const auto* entry = m_projectManager->findEntry(clip.filepath);
    if (!entry) return {};
    return entry->inputColorSpaceOverride;
}

float PlaybackTimeAuthority::computeSectionFadeMultiplier(const Clip& clip) const {
    if (!m_timeline) return 1.0f;
    const auto& sections = m_timeline->getSections();
    if (sections.empty()) return 1.0f;

    const FrameNumber currentFrame = m_timeline->getCurrentFrame();
    const FrameNumber clipStart    = clip.startFrame;
    const FrameNumber clipEnd      = clip.startFrame + clip.duration;
    const double timelineFrameRate = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;
    // ±1 timeline-frame snap tolerance — matches the boundary semantics
    // documented in the Phase D plan; clips that merely cross a break
    // (>1 frame off either end) get no envelope.
    const FrameNumber snapTol = 1;

    float multiplier = 1.0f;

    auto absDiff = [](FrameNumber a, FrameNumber b) -> FrameNumber {
        return a >= b ? a - b : b - a;
    };

    // [SBG] diag — flag clips whose start or end aligns with any section
    // break, so the bottom return can log a single line per call. Volume
    // stays bounded (only break-aligned clips are noisy). REMOVE after
    // section-break-glitch fix lands.
    bool sbgClipAlignedWithBreak = false;
    for (const auto& sec : sections) {
        const FrameNumber bf = static_cast<FrameNumber>(
            (static_cast<double>(sec.breakFrame) * timelineFrameRate) / 1000000.0);
        if (absDiff(clipStart, bf) <= snapTol || absDiff(clipEnd, bf) <= snapTol) {
            sbgClipAlignedWithBreak = true;
            break;
        }
    }

    // At-break visibility gate. Clips that START at the current break stay
    // invisible until GO, regardless of fadeSeconds. User requirement:
    // "things after the break shouldn't start yet until we resume."
    // Authored fade-in (if any) resumes naturally post-GO once both
    // conditions below clear.
    //
    // The gate fires in two situations:
    //   (a) SectionScheduler has latched at-break (sectionAtBreak() == true)
    //       — the common case during park.
    //   (b) State is Playing AND currentFrame == breakFrame — catches the
    //       brief window between Timeline crossing the break and
    //       SectionScheduler::tick latching atBreak. Without this, the
    //       break-aligned clip flashes at full opacity for 1-3 ticks
    //       (the "blip" bug). Excluding Paused here keeps scrub-to-break
    //       editing — where state is Paused after Timeline::seek and
    //       sectionAtBreak() is false — showing the clip's first frame.
    {
        const bool atBreakLatched = m_timeline->sectionAtBreak();
        const bool isPlaying =
            m_timeline->getPlaybackState() == PlaybackState::Playing;
        if (atBreakLatched || isPlaying) {
            constexpr FrameNumber gateSnapTol = 1;
            auto absDiffGate = [](FrameNumber a, FrameNumber b) -> FrameNumber {
                return a >= b ? a - b : b - a;
            };
            for (const auto& sec : sections) {
                const FrameNumber breakFrame = static_cast<FrameNumber>(
                    (static_cast<double>(sec.breakFrame) * timelineFrameRate) / 1000000.0);
                if (breakFrame == currentFrame &&
                    absDiffGate(clip.startFrame, breakFrame) <= gateSnapTol) {
                    multiplier = 0.0f;
                    break;
                }
            }
        }
    }

    for (const auto& sec : sections) {
        if (sec.fadeSeconds <= 0.0) continue;
        const FrameNumber breakFrame = static_cast<FrameNumber>(
            (static_cast<double>(sec.breakFrame) * timelineFrameRate) / 1000000.0);
        const FrameNumber fadeFrames = static_cast<FrameNumber>(
            std::ceil(sec.fadeSeconds * timelineFrameRate));
        if (fadeFrames <= 0) continue;

        // Fade in: clip's start coincides with this break.
        if (absDiff(breakFrame, clipStart) <= snapTol) {
            if (currentFrame >= clipStart &&
                currentFrame < clipStart + fadeFrames) {
                const float t = static_cast<float>(currentFrame - clipStart)
                              / static_cast<float>(fadeFrames);
                // min-combine: a clip with both ends on breaks (shorter
                // than fadeIn + fadeOut) takes the more restrictive ramp.
                multiplier = std::min(multiplier, t);
            }
        }
        // Phase 6 — fade-out is the post-end held tail. Window is
        // [clipEnd, clipEnd + fadeFrames). At currentFrame == clipEnd the
        // multiplier is 1.0 (fully visible at the break, matches the
        // at-break held look), ramping linearly to 0.0 at the window's
        // open upper edge. The post-end window can't overlap the
        // at-start fade-in window of the *same* clip, so the min-combine
        // for both-aligned clips still works cleanly.
        if (absDiff(breakFrame, clipEnd) <= snapTol) {
            if (currentFrame >= clipEnd &&
                currentFrame < clipEnd + fadeFrames) {
                const float t = 1.0f - static_cast<float>(currentFrame - clipEnd)
                                      / static_cast<float>(fadeFrames);
                multiplier = std::min(multiplier, t);
            }
        }
    }
    const float sbgResult = std::clamp(multiplier, 0.0f, 1.0f);
    // [SBG] diag — REMOVE after section-break-glitch fix lands.
    if (sbgClipAlignedWithBreak) {
        std::cout << "[SBG][gate] clipStart=" << clipStart
                  << " clipEnd=" << clipEnd
                  << " currentFrame=" << currentFrame
                  << " atBreak=" << (m_timeline->sectionAtBreak() ? 1 : 0)
                  << " mult=" << sbgResult
                  << std::endl;
    }
    return sbgResult;
}

void PlaybackTimeAuthority::buildActiveSet(std::vector<ActiveClip>& out) const {
    out.clear();
    if (!m_timeline) return;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    auto view = m_registry.view<Clip, VideoTexture>();
    for (auto [entity, clip, videoTex] : view.each()) {
        if (!videoTex.isAllocated()) continue;
        if (!isClipActiveAtFrame(clip, currentFrame)) continue;

        ActiveClip ac;
        ac.entity         = entity;
        ac.descriptorSlot = videoTex.descriptorSlot;
        ac.mediaFrame     = mapToMediaFrame(entity, clip, currentFrame);
        ac.ocioOverride   = lookupInputColorSpaceOverride(clip);
        out.push_back(std::move(ac));
    }
}

void PlaybackTimeAuthority::buildSceneSnapshot(bus::SceneSnapshot& out) const {
    // Stage primitives baked here: Screens, MappingSurfaces, Projectors,
    // OutputDisplays, ClipCatalog. Props (Prop component) are intentionally
    // NOT baked — they exist only for editor pre-vis (see Prop.hpp). The
    // show thread never reads them, so they can't drift the output.
    // If/when projector masking lands (Phase 2), it adds a separate
    // propCatalog field rather than folding props into screens.
    // Build OA fold-in lookup: for each Screen entity, find the highest-priority
    // (lowest trackIndex) active OA layer that targets it and has override flags set.
    // Lower trackIndex wins (v1 deterministic multi-layer policy; see Phase 3.5 for UI).
    FrameNumber currentFrameForOA = m_timeline ? m_timeline->getCurrentFrame() : 0;
    auto oaScan = m_registry.view<ObjectAnimationLayer, ObjectAnimationOutput, Layer>();

    out.screens.clear();
    for (auto [entity, screen] : m_registry.view<Screen>().each()) {
        bus::ScreenSnapshot ss;
        ss.entity            = static_cast<std::uint64_t>(entity);
        ss.name              = screen.name;
        ss.visible           = screen.visible;
        ss.width             = screen.width;
        ss.height            = screen.height;
        ss.renderTargetSlot  = screen.renderTargetSlot;
        ss.renderTargetValid = screen.renderTargetValid;
        ss.position          = screen.position;
        ss.rotation          = screen.rotation;

        // Convert Screen::size (meters) → effective vertex multiplier on the
        // editor side so the show thread / OutputManager can multiply
        // verts by ss.scale directly without needing mesh bounds across
        // the bus (Model meshes are intentionally not snapshotted).
        const MeshData* mesh = nullptr;
        if (screen.modelEntity != entt::null && m_registry.valid(screen.modelEntity)) {
            if (const auto* m = m_registry.try_get<Model>(screen.modelEntity))
                if (m->mesh.isValid()) mesh = &m->mesh;
        }
        std::array<float, 3> effectiveSize = screen.size;
        ss.scale = computeEffectiveScale(effectiveSize, mesh);
        ss.modelEntity = entityToU64(screen.modelEntity);

        // Fold in ObjectAnimationOutput from active OA layers targeting this screen.
        // Multiple layers may target the same screen; lower trackIndex wins (v1 policy).
        // Belt-and-suspenders active-frame guard here mirrors AnimationSystem's reset.
        const ObjectAnimationOutput* winningOA = nullptr;
        uint32_t winningTrackIndex = UINT32_MAX;
        for (auto [oaEntity, oal, oaOut, oaLayer] : oaScan.each()) {
            if (oal.target != entity) continue;
            if (currentFrameForOA < oaLayer.startFrame ||
                currentFrameForOA >= oaLayer.startFrame + oaLayer.duration) continue;
            // Equal trackIndex: first-creation-order wins. Create OA layers in priority order to avoid ambiguity.
            if (oaLayer.trackIndex < winningTrackIndex) {
                winningTrackIndex = oaLayer.trackIndex;
                winningOA = &oaOut;
            }
        }
        if (winningOA) {
            if (winningOA->hasPosition) ss.position = winningOA->positionOverride;
            if (winningOA->hasRotation) ss.rotation = winningOA->rotationOverride;
            if (winningOA->hasSize) {
                effectiveSize = winningOA->sizeOverride;
                ss.scale = computeEffectiveScale(effectiveSize, mesh);
            }
        }

        out.screens.push_back(std::move(ss));
    }

    out.surfaces.clear();
    for (auto [entity, surf] : m_registry.view<MappingSurface>().each()) {
        bus::MappingSurfaceSnapshot ms;
        ms.entity       = static_cast<std::uint64_t>(entity);
        ms.visible      = surf.visible;
        ms.outputIndex  = surf.outputIndex;
        ms.surfaceIndex = surf.surfaceIndex;
        for (int i = 0; i < 4; ++i) {
            ms.corners[i]   = {surf.corners[i].x,   surf.corners[i].y};
            ms.sourceUVs[i] = {surf.sourceUVs[i].x, surf.sourceUVs[i].y};
        }
        ms.softEdgeLeft   = surf.softEdge.left;
        ms.softEdgeRight  = surf.softEdge.right;
        ms.softEdgeTop    = surf.softEdge.top;
        ms.softEdgeBottom = surf.softEdge.bottom;
        ms.brightness     = surf.brightness;
        ms.gamma          = surf.gamma;
        out.surfaces.push_back(std::move(ms));
    }

    out.projectors.clear();
    for (auto [entity, proj] : m_registry.view<Projector>().each()) {
        bus::ProjectorSnapshot ps;
        ps.entity           = static_cast<std::uint64_t>(entity);
        ps.position         = proj.position;
        ps.rotation         = proj.rotation;
        ps.fovDegrees       = proj.fovDegrees;
        ps.nearClip         = proj.nearClip;
        ps.farClip          = proj.farClip;
        ps.distortionK1     = proj.distortionK1;
        ps.distortionK2     = proj.distortionK2;
        ps.useResidualWarp  = proj.useResidualWarp;
        ps.isCalibrated     = proj.isCalibrated;
        ps.targetSurfaceCount = proj.targetSurfaceCount;
        for (int i = 0; i < proj.targetSurfaceCount && i < Projector::MAX_TARGETS; ++i)
            ps.targetSurfaces[i] = entityToU64(proj.targetSurfaces[i]);
        for (const auto& cp : proj.calibrationPoints) {
            bus::CalibrationPointSnapshot cps;
            cps.worldPos    = cp.worldPos;
            cps.projectorUV = cp.projectorUV;
            ps.calibrationPoints.push_back(std::move(cps));
        }
        out.projectors.push_back(std::move(ps));
    }

    out.outputs.clear();
    for (auto [entity, od] : m_registry.view<OutputDisplay>().each()) {
        bus::OutputSnapshot os;
        os.entity               = static_cast<std::uint64_t>(entity);
        os.name                 = od.name;
        os.outputType           = static_cast<int>(od.type);
        os.enabled              = od.enabled;
        os.isPhysical           = od.isPhysical();
        os.outputWindowSlot     = od.outputWindowSlot;
        os.physicalDisplayIndex = od.physicalDisplayIndex;
        os.width                = od.width;
        os.height               = od.height;
        os.inputRegionX         = od.inputRegion.x;
        os.inputRegionY         = od.inputRegion.y;
        os.inputRegionWidth     = od.inputRegion.width;
        os.inputRegionHeight    = od.inputRegion.height;
        os.brightness           = od.brightness;
        os.gamma                = od.gamma;
        os.sourceProjector      = entityToU64(od.sourceProjector);
        os.sourceScreen         = entityToU64(od.sourceScreen);
        {
            const auto& src = od.calibrationOverlay;
            auto& dst = os.calibrationOverlay;
            dst.enabled         = src.enabled;
            dst.numPoints       = src.numPoints;
            dst.activeIndex     = src.activeIndex;
            dst.precisionCursor = src.precisionCursor;
            for (size_t i = 0; i < src.points.size() && i < dst.points.size(); ++i) {
                dst.points[i][0] = src.points[i].x;
                dst.points[i][1] = src.points[i].y;
            }
        }
        os.windowX              = od.windowX;
        os.windowY              = od.windowY;
        os.outputIndex          = od.outputIndex;
        out.outputs.push_back(std::move(os));
    }

    // Clip catalog — all registry reads for clip/layer/phase state happen here
    // on the editor thread. The show thread reads only out.clipCatalog.
    out.clipCatalog.clear();
    for (auto [entity, clip, videoTex] : m_registry.view<Clip, VideoTexture>().each()) {
        if (!videoTex.isAllocated()) continue;

        bus::ClipCatalogEntry ce;
        ce.entity          = static_cast<std::uint64_t>(entity);
        ce.startFrame      = clip.startFrame;
        ce.duration        = clip.duration;
        ce.mediaStartFrame = clip.mediaStartFrame;
        ce.mediaOutFrame   = clip.mediaOutFrame;
        ce.framerate       = clip.framerate;
        ce.playbackMode    = static_cast<int>(clip.playbackMode);
        ce.sectionBehavior = static_cast<int>(clip.sectionBehavior);
        ce.descriptorSlot  = static_cast<int>(videoTex.descriptorSlot);

        // Bake transform matrix on the editor thread (no const_cast needed here).
        // Also bake the individual axes (position/rotation/scale) so the show
        // thread can re-evaluate animation per render frame without reading
        // the registry — see NEW-07 fix in applyBakedAnimation().
        if (auto* t = m_registry.try_get<Transform>(entity)) {
            t->updateMatrix();
            std::memcpy(ce.transformMatrix.data(),
                        glm::value_ptr(t->matrix),
                        sizeof(ce.transformMatrix));
            ce.position = { t->position.x, t->position.y, t->position.z };
            ce.rotation = { t->rotation.x, t->rotation.y, t->rotation.z };
            ce.scale    = { t->scale.x,    t->scale.y,    t->scale.z    };
        }

        if (auto* layer = m_registry.try_get<MediaLayer>(entity)) {
            ce.opacity   = layer->getOpacity();
            ce.blendMode = static_cast<int>(layer->blendMode);
            ce.zOrder    = layer->zOrder;
        }

        // AnimatedProperties → BakedTrack snapshot. Show thread evaluates
        // these at currentFrame each render tick so animation stays alive
        // during editor stalls (NEW-07). Skipped (empty vector) for clips
        // without animation; show side then falls through to the static
        // ce.transformMatrix / ce.opacity.
        if (const auto* anim = m_registry.try_get<AnimatedProperties>(entity);
            anim && anim->hasAnyKeyframes())
        {
            ce.tracks.reserve(anim->tracks.size());
            for (const auto& src : anim->tracks) {
                if (!src.hasKeyframes()) continue;
                bus::BakedTrack dst;
                dst.property = static_cast<int>(src.property);
                dst.enabled  = src.enabled;
                dst.keyframes.reserve(src.keyframes.size());
                for (const auto& sk : src.keyframes) {
                    bus::BakedKeyframe k;
                    k.frame         = sk.frame;
                    k.value         = sk.value;
                    k.interpolation = static_cast<int>(sk.interpolation);
                    k.easeIn        = sk.easeIn;
                    k.easeOut       = sk.easeOut;
                    dst.keyframes.push_back(k);
                }
                ce.tracks.push_back(std::move(dst));
            }
        }

        ce.targetScreen = (clip.targetScreen == entt::null)
            ? UINT64_MAX
            : entityToU64(clip.targetScreen);

        ce.ocioOverride = lookupInputColorSpaceOverride(clip);

        if (const auto* phase = m_registry.try_get<ClipPlaybackPhase>(entity)) {
            ce.hasPhase                  = true;
            ce.phase_inContinuation      = phase->inContinuation;
            ce.phase_sourcePhaseFrames   = phase->sourcePhaseFrames;
            ce.phase_tailHoldMediaFrame  = phase->tailHoldMediaFrame;
            ce.phase_postBreakMediaAnchor = phase->postBreakMediaAnchor;
            ce.phase_anchorTimelineFrame  = phase->anchorTimelineFrame;
        }

        out.clipCatalog.push_back(std::move(ce));
    }

    // OA layer catalog — all OA layers with AnimatedProperties baked into the
    // snapshot so the show thread can re-evaluate tracks per render frame and
    // fold results into ScreenSnapshot position/rotation/scale during editor
    // stalls (mirrors the NEW-07 clip-animation fix via applyBakedAnimation).
    // Layers without keyframes are omitted — the static fold-in above via
    // ObjectAnimationOutput is sufficient for those.
    out.objectAnimationLayers.clear();
    for (auto [oaEntity, oal, oaLayer] : m_registry.view<ObjectAnimationLayer, Layer>().each()) {
        // Mirror the 3.4 fold-in gate: only active layers go on the bus.
        if (currentFrameForOA < oaLayer.startFrame ||
            currentFrameForOA >= oaLayer.startFrame + oaLayer.duration) continue;
        const auto* anim = m_registry.try_get<AnimatedProperties>(oaEntity);
        if (!anim || !anim->hasAnyKeyframes()) continue;

        bus::ObjectAnimationLayerSnapshot oas;
        oas.targetScreen = (oal.target == entt::null)
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(oal.target);
        oas.startFrame  = oaLayer.startFrame;
        oas.duration    = oaLayer.duration;
        oas.trackIndex  = oaLayer.trackIndex;

        oas.tracks.reserve(anim->tracks.size());
        for (const auto& src : anim->tracks) {
            if (!src.hasKeyframes()) continue;
            bus::BakedTrack dst;
            dst.property = static_cast<int>(src.property);
            dst.enabled  = src.enabled;
            dst.keyframes.reserve(src.keyframes.size());
            for (const auto& sk : src.keyframes) {
                bus::BakedKeyframe k;
                k.frame         = sk.frame;
                k.value         = sk.value;
                k.interpolation = static_cast<int>(sk.interpolation);
                k.easeIn        = sk.easeIn;
                k.easeOut       = sk.easeOut;
                dst.keyframes.push_back(k);
            }
            oas.tracks.push_back(std::move(dst));
        }
        if (!oas.tracks.empty())
            out.objectAnimationLayers.push_back(std::move(oas));
    }

    // Generative layer catalog — Muncher and (eventually) other procedural
    // kinds. Filter to currently-active timeline frames here so the show
    // thread doesn't need to re-check start/duration. ADR-0014: editor
    // thread is the sole reader of GenerativeLayer / MunchersGameState.
    out.generativeLayers.clear();
    for (auto [entity, layer, gen] :
            m_registry.view<Layer, GenerativeLayer>().each()) {
        if (currentFrameForOA < layer.startFrame ||
            currentFrameForOA >= layer.startFrame + layer.duration) continue;

        bus::GenerativeLayerSnapshot snap;
        snap.entity       = static_cast<std::uint64_t>(entity);
        snap.targetScreen = (gen.targetScreen == entt::null)
                              ? std::uint64_t{UINT64_MAX}
                              : static_cast<std::uint64_t>(gen.targetScreen);

        if (const auto* ml = m_registry.try_get<MediaLayer>(entity)) {
            snap.opacity   = ml->opacity;
            snap.blendMode = static_cast<int>(ml->blendMode);
            snap.zOrder    = ml->zOrder;
        }

        // UV-space transform — same bake pattern as ClipCatalogEntry.
        if (auto* t = m_registry.try_get<Transform>(entity)) {
            t->updateMatrix();
            std::memcpy(snap.transformMatrix.data(),
                        glm::value_ptr(t->matrix),
                        sizeof(snap.transformMatrix));
        }

        // Carry the slot the show thread previously allocated for this
        // layer (R2D ack wrote it on the editor thread). -1 if not yet
        // allocated — PASS 2 compositor skips the blit until then.
        snap.renderTargetSlot = gen.renderTargetSlot;

        // Sub-kind dispatch by component composition (ADR-0016) — same rule
        // GenerativeSystem uses on the editor side.
        if (const auto* mgs = m_registry.try_get<MunchersGameState>(entity)) {
            snap.kind             = bus::GenerativeLayerSnapshot::Kind::Muncher;
            snap.muncher_simFrame = mgs->simFrame;
            snap.muncher_x        = mgs->muncherX;
            snap.muncher_y        = mgs->muncherY;
            snap.muncher_inputX   = mgs->inputX;
            snap.muncher_inputY   = mgs->inputY;
            for (int gi = 0; gi < MunchersGameState::kNumGhosts; ++gi) {
                snap.muncher_ghosts[gi * 2 + 0] = mgs->ghosts[gi].x;
                snap.muncher_ghosts[gi * 2 + 1] = mgs->ghosts[gi].y;
            }
            snap.muncher_wallBits   = mgs->wallBits;
            snap.muncher_pelletBits = mgs->pelletBits;
            snap.muncher_score      = mgs->score;
            snap.muncher_lives      = mgs->lives;
        } else {
            // No kind-specific state component → skip (defensive — the
            // editor-side creation paths always attach one).
            continue;
        }

        out.generativeLayers.push_back(std::move(snap));
    }

    // Effect-chain bake — one LayerEffectsSnapshot per entity that
    // carries an EffectChain. Show side looks this up by entity when
    // building each ContentLayerSnapshot in buildRenderFrame and folds
    // in `effects` + the ping-pong RT slots that PASS 1.5 needs.
    // Issue #54.
    //
    // paramBlob layout matches shaders/effects/_effect_common.hlsli:
    // 14 vec4 slots of params + 16 bytes of misc (viewport size filled
    // in by the renderer at drawEffectPass time) + padding to 256.
    constexpr std::size_t kEffectBlobBytes = 256;
    constexpr std::size_t kEffectSlotBytes = 16;

    const FrameNumber currentFrame = m_timeline ? m_timeline->getCurrentFrame() : 0;

    out.layerEffects.clear();
    for (auto [layerEntity, chain] : m_registry.view<EffectChain>().each()) {
        bus::LayerEffectsSnapshot lfx;
        lfx.entity = entityToU64(layerEntity);

        if (const auto* rts = m_registry.try_get<EffectChainRenderTargets>(layerEntity)) {
            lfx.slotA  = rts->slotA;
            lfx.slotB  = rts->slotB;
            lfx.width  = rts->width;
            lfx.height = rts->height;
        }

        // clip-local frame for keyframe evaluation. Mirrors AnimationSystem
        // (track frames are clip-relative). Fall back to absolute frame for
        // entities without a Clip component (e.g. future generative layers).
        FrameNumber localFrame = currentFrame;
        if (const auto* clip = m_registry.try_get<Clip>(layerEntity)) {
            localFrame = (currentFrame >= clip->startFrame)
                            ? (currentFrame - clip->startFrame)
                            : FrameNumber{0};
        }

        lfx.effects.reserve(chain.nodes.size());
        for (auto effectEnt : chain.nodes) {
            if (!m_registry.valid(effectEnt)) continue;
            const auto* fx = m_registry.try_get<Effect>(effectEnt);
            if (!fx) continue;

            bus::EffectSnapshot es;
            es.kindIdHash = fx->kindId;
            es.enabled    = fx->enabled;
            es.paramBlob.assign(kEffectBlobBytes, std::uint8_t{0});

            // Look up the kind so we know each param's slot index +
            // type. Missing kind = registry not bound or unknown effect
            // → ship the snapshot with an empty blob so the renderer
            // can log + skip.
            const effects::EffectKind* kind = m_effectKindRegistry
                ? m_effectKindRegistry->find(fx->kindId)
                : nullptr;

            if (kind) {
                const auto* params =
                    m_registry.try_get<EffectParameters>(effectEnt);
                const auto* anim =
                    m_registry.try_get<EffectAnimatedParameters>(effectEnt);

                // 1) Marshal each schema slot from the static value
                //    (EffectParameters.values[N] preferred, schema
                //    defaultValue as fallback). Slot 0 → bytes 0..15,
                //    slot N → bytes N*16..N*16+15.
                const std::size_t maxSchemaSlots =
                    std::min(kind->params.size(), std::size_t{14});
                for (std::size_t slotN = 0; slotN < maxSchemaSlots; ++slotN) {
                    const auto& schema = kind->params[slotN];
                    ParamValue value = schema.defaultValue;
                    if (params && slotN < params->values.size()) {
                        value = params->values[slotN];
                    }
                    marshalParamValueToSlot(value,
                        es.paramBlob.data() + slotN * kEffectSlotBytes);
                }

                // 2) Animation overrides — evaluate tracks at the editor's
                //    clip-local frame and overwrite the matching slot.
                //    Also copies the track into BakedEffectTrack with the
                //    resolved paramName so the show side can re-evaluate
                //    during editor stalls (Phase 3 polish).
                if (anim) {
                    es.tracks.reserve(anim->tracks.size());
                    for (const auto& src : anim->tracks) {
                        if (!src.enabled || src.keyframes.empty()) continue;

                        // Find matching ParamSchema by hash.
                        std::size_t slotN = SIZE_MAX;
                        for (std::size_t i = 0; i < maxSchemaSlots; ++i) {
                            const auto& schema = kind->params[i];
                            if (effects::fnv1a32(schema.name) == src.paramKeyHash) {
                                slotN = i;
                                break;
                            }
                        }
                        if (slotN == SIZE_MAX) continue;

                        const auto& schema = kind->params[slotN];
                        const float def =
                            schema.defaultValue.type == ParamValue::Type::Float
                                ? schema.defaultValue.f4[0]
                                : 0.0f;
                        const float v = evaluateInMemoryKeyframes(
                            src.keyframes, localFrame, def);

                        // v1: only Float params are animatable; write to
                        // the slot's .x. Vec2/Vec3/Color animation lands
                        // when component-tracks ship in Phase 3.
                        std::memcpy(es.paramBlob.data() + slotN * kEffectSlotBytes,
                                    &v, sizeof(float));

                        bus::BakedEffectTrack dst;
                        dst.paramName = schema.name;
                        dst.enabled   = src.enabled;
                        dst.keyframes.reserve(src.keyframes.size());
                        for (const auto& sk : src.keyframes) {
                            bus::BakedKeyframe k;
                            k.frame         = sk.frame;
                            k.value         = sk.value;
                            k.interpolation = static_cast<int>(sk.interpolation);
                            k.easeIn        = sk.easeIn;
                            k.easeOut       = sk.easeOut;
                            dst.keyframes.push_back(k);
                        }
                        es.tracks.push_back(std::move(dst));
                    }
                }
            }

            lfx.effects.push_back(std::move(es));
        }

        out.layerEffects.push_back(std::move(lfx));
    }
}

void PlaybackTimeAuthority::buildRenderFrame(bus::RenderFrame& out,
                                              const bus::SceneSnapshot& scene) const {
    out.activeClips.clear();
    out.wantedFrames.clear();
    out.frameNumber = 0;
    out.deltaTime   = m_deltaTime;
    out.playState   = TransportState::Stopped;

    // Merge scene snapshot (captured editor-side) into the RenderFrame.
    out.screens          = scene.screens;
    out.surfaces         = scene.surfaces;
    out.projectors       = scene.projectors;
    out.outputs          = scene.outputs;
    out.generativeLayers = scene.generativeLayers;

    if (!m_timeline) return;

    const FrameNumber currentFrame = m_timeline->getCurrentFrame();
    out.frameNumber = currentFrame;
    out.playState   = toTransportState(m_timeline->getPlaybackState());
    const double timelineFrameRate = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;

    // Show thread reads ONLY the clip catalog — zero registry access.
    for (const auto& ce : scene.clipCatalog) {
        const Clip clip = clipFromCatalog(ce);
        if (!isClipActiveAtFrame(clip, currentFrame)) continue;

        bus::ClipRenderState c;
        c.entity       = ce.entity;
        c.slot         = ce.descriptorSlot;
        c.mediaFrame   = mapToMediaFrameFromCatalog(ce, currentFrame, timelineFrameRate);
        c.ocioOverride = ce.ocioOverride;
        c.transformMatrix = ce.transformMatrix;
        c.opacity      = ce.opacity;
        c.blendMode    = static_cast<BlendMode>(ce.blendMode);
        c.zOrder       = ce.zOrder;
        c.targetScreen = ce.targetScreen;
        c.sectionFadeMultiplier = computeSectionFadeMultiplier(clip);

        // NEW-07: re-evaluate animation tracks at the show thread's current
        // frame so opacity / transform stay alive while the editor stalls.
        // No-op (early return) for clips without baked tracks.
        applyBakedAnimation(ce, currentFrame, c);

        out.activeClips.push_back(std::move(c));
    }

    // OA stall-safety: re-evaluate baked OA tracks per render frame and fold
    // results into the ScreenSnapshot entries in out.screens. This mirrors
    // what buildSceneSnapshot does on the editor side via ObjectAnimationOutput,
    // but runs on the show thread so animation survives editor stalls.
    //
    // Priority: lower trackIndex wins when multiple OA layers target the same
    // screen — matches the editor-side fold-in policy.
    for (auto& ss : out.screens) {
        const bus::ObjectAnimationLayerSnapshot* winning = nullptr;
        for (const auto& oa : scene.objectAnimationLayers) {
            if (oa.targetScreen != ss.entity) continue;
            if (currentFrame < oa.startFrame || currentFrame >= oa.startFrame + oa.duration) continue;
            if (!winning || oa.trackIndex < winning->trackIndex) winning = &oa;
        }
        if (!winning) continue;

        const FrameNumber localFrame =
            (currentFrame >= winning->startFrame)
                ? (currentFrame - winning->startFrame)
                : FrameNumber{0};

        for (const auto& track : winning->tracks) {
            if (!track.enabled || track.keyframes.empty()) continue;
            const float v = evaluateBakedTrack(track, localFrame);
            switch (static_cast<AnimatableProperty>(track.property)) {
                case AnimatableProperty::PositionX: ss.position[0] = v; break;
                case AnimatableProperty::PositionY: ss.position[1] = v; break;
                case AnimatableProperty::PositionZ: ss.position[2] = v; break;
                case AnimatableProperty::Rotation:
                case AnimatableProperty::RotationY: ss.rotation[1] = v; break;
                case AnimatableProperty::RotationX: ss.rotation[0] = v; break;
                case AnimatableProperty::ScaleX:    ss.scale[0]    = v; break;
                case AnimatableProperty::ScaleY:    ss.scale[1]    = v; break;
                case AnimatableProperty::ScaleZ:    ss.scale[2]    = v; break;
                default: break;
            }
        }
    }

    // Unified content-layer list for PASS 2 compositor. Built on the show
    // thread from activeClips (already carries section fade + post-animation
    // transform/opacity) and generativeLayers (carries the editor-baked
    // transform + renderTargetSlot). Future content kinds plug in here.
    out.contentLayers.clear();
    out.contentLayers.reserve(out.activeClips.size() + out.generativeLayers.size());

    // Side-table lookup for per-layer effects (issue #54). Linear scan
    // per content layer — both vectors are tiny (<20 entries each in
    // realistic projects). Phase 1: copies effects + tracks across; the
    // show-side animation re-evaluation arrives in Phase 2 alongside the
    // EffectKindRegistry that owns parameter offsets.
    auto findLayerEffects = [&scene](std::uint64_t entity) -> const bus::LayerEffectsSnapshot* {
        for (const auto& le : scene.layerEffects) {
            if (le.entity == entity) return &le;
        }
        return nullptr;
    };

    for (const auto& ac : out.activeClips) {
        bus::ContentLayerSnapshot c;
        c.entity                = ac.entity;
        c.targetScreen          = ac.targetScreen;
        c.transformMatrix       = ac.transformMatrix;
        c.opacity               = ac.opacity;
        c.sectionFadeMultiplier = ac.sectionFadeMultiplier;
        c.blendMode             = static_cast<int>(ac.blendMode);
        c.zOrder                = ac.zOrder;
        c.sourceKind            = bus::ContentLayerSnapshot::SourceKind::Video;
        c.sourceSlot            = ac.slot;
        // colorSpace/ocioColorSpace: compositor resolves via PlaybackPresenter
        // for Video-source layers (per-entity cached on the show side).
        if (const auto* le = findLayerEffects(ac.entity)) {
            c.effects          = le->effects;
            c.effectChainSlotA = le->slotA;
            c.effectChainSlotB = le->slotB;
            // postEffectsSlot stays -1 here — PASS 1.5 fills it after it
            // actually draws the chain.
        }
        out.contentLayers.push_back(c);
    }

    for (const auto& gl : out.generativeLayers) {
        bus::ContentLayerSnapshot c;
        c.entity                = gl.entity;
        c.targetScreen          = gl.targetScreen;
        c.transformMatrix       = gl.transformMatrix;
        c.opacity               = gl.opacity;
        c.sectionFadeMultiplier = 1.0f;  // SectionScheduler integration is NEW-08
        c.blendMode             = gl.blendMode;
        c.zOrder                = gl.zOrder;
        c.sourceKind            = bus::ContentLayerSnapshot::SourceKind::Compose;
        c.sourceSlot            = gl.renderTargetSlot;
        // colorSpace stays Linear (default 0) — the PASS 1 RT is in linear-light.
        if (const auto* le = findLayerEffects(gl.entity)) {
            c.effects          = le->effects;
            c.effectChainSlotA = le->slotA;
            c.effectChainSlotB = le->slotB;
        }
        out.contentLayers.push_back(c);
    }

    std::sort(out.contentLayers.begin(), out.contentLayers.end(),
              [](const bus::ContentLayerSnapshot& a, const bus::ContentLayerSnapshot& b) {
                  return a.zOrder < b.zOrder;
              });
}

void PlaybackTimeAuthority::buildRenderFrame(bus::RenderFrame& out) const {
    // Legacy single-call path: build scene snapshot inline.
    bus::SceneSnapshot scene;
    buildSceneSnapshot(scene);
    buildRenderFrame(out, scene);
}

} // namespace entity
