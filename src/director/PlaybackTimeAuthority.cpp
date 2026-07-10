#include "entity/director/PlaybackTimeAuthority.hpp"

#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/RemotePatch.hpp"
#include "entity/remote/RemoteControlStore.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectChainRenderTargets.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/MunchersGameState.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TextLayerState.hpp"
#include "entity/components/OutputSurface.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/director/CatalogClipMath.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/timeline/SectionFade.hpp"
#include "entity/timeline/Timeline.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
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

// Resolve a layer's `ContentRoutingRef` → `ContentRoutingAsset` for the
// bake (ADR-0022). Returns the first target's screen, with UINT64_MAX
// meaning "render on all visible." `legacyTargetScreen` is the deprecated
// `Clip::targetScreen` / `GenerativeLayer::targetScreen` alias kept on the
// bus snapshot for one wire-format version; the live bake uses
// ContentRoutingRef as the source of truth.
inline const ContentRoutingAsset*
tryResolveAsset(const entt::registry& reg, entt::entity layerEntity) {
    const auto* ref = reg.try_get<ContentRoutingRef>(layerEntity);
    if (!ref) return nullptr;
    if (ref->asset == entt::null) return nullptr;
    if (!reg.valid(ref->asset)) return nullptr;
    return reg.try_get<ContentRoutingAsset>(ref->asset);
}

inline std::uint64_t resolveTargetScreen(const entt::registry& reg,
                                          entt::entity layerEntity,
                                          entt::entity legacyTargetScreen) {
    if (const auto* asset = tryResolveAsset(reg, layerEntity)) {
        if (asset->targets.empty()) {
            return UINT64_MAX;
        }
        const entt::entity screen = asset->targets.front().screen;
        return (screen == entt::null) ? UINT64_MAX : entityToU64(screen);
    }
    // No ref / null asset = legacy "render on all visible" semantic. Fall
    // back to the deprecated single-screen alias one more time so v19
    // projects loaded before the ContentRoutingRef migration drain still
    // render their old single target.
    return (legacyTargetScreen == entt::null)
        ? UINT64_MAX
        : entityToU64(legacyTargetScreen);
}

// Bake a layer's resolved routing → bus route list (ADR-0021 wire format,
// ADR-0022 resolution path). `mode` mirrors entity::RouteMode (0=Direct,
// 1=Tiled). Empty `targets` is left empty so the show side expands it to
// "render on every visible screen" using out.screens. When no ref/asset
// is reachable (a newly-created clip whose ref is still null, a v19
// project loaded before migration), returns empty + mode=0 and the show
// side falls back to the legacy targetScreen alias.
inline void bakeRoutes(const entt::registry& reg,
                        entt::entity layerEntity,
                        std::vector<bus::ContentLayerRoute>& outRoutes,
                        int& outMode) {
    outRoutes.clear();
    outMode = 0;
    const auto* asset = tryResolveAsset(reg, layerEntity);
    if (!asset) return;
    outMode = static_cast<int>(asset->kind);
    outRoutes.reserve(asset->targets.size());
    for (const auto& t : asset->targets) {
        bus::ContentLayerRoute r;
        r.screen = (t.screen == entt::null) ? UINT64_MAX : entityToU64(t.screen);
        r.uvRect = t.uvRect;
        outRoutes.push_back(r);
    }
}

// clipFromCatalog + mapToMediaFrameFromCatalog were hoisted to
// entity/director/CatalogClipMath.{hpp,cpp} (issue #74) so the
// DecodeSystem/AudioSystem stall-fallback ticks can share them.
// buildRenderFrame below still calls them unqualified — same namespace.
//
// The Normal-mode break-aligned extension math lives in Clip.hpp as
// the free function `entity::computeExtendedDuration(clip, fps,
// endAlignsWithBreak, endingBreakFadeSeconds)`. DecodeSystem and
// AudioSystem call the same helper so their inTail-versus-extension
// decisions stay in lockstep with PTA. See ADR-0012 amendment
// 2026-05-23 + the 2026-05-23 follow-up.

// -- Show-thread animation evaluator (NEW-07) ------------------------------
//
// The show thread evaluates `bus::BakedTrack`s out of the SceneSnapshot at
// each render frame so animation stays alive while the editor thread is
// stalled (and therefore not re-baking new snapshots / re-ticking
// AnimationSystem). Math mirrors entity::KeyframeTrack::evaluate in
// include/entity/components/AnimatedProperties.hpp — keep them in sync.

// In-memory keyframe evaluation moved to a public free function
// `entity::evaluateKeyframes` in AnimatedProperties.hpp — shared with
// AnimationSystem now that editor-side effect-param eval also needs it.
// The show-side re-evaluator on `bus::BakedKeyframe` is still
// `interpolateBakedKeyframes` below — same math, different wire form.

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

// Copy AnimatedProperties keyframe tracks into the bus BakedTrack form.
// Shared by the clip / OA / generative bake sites in buildSceneSnapshot.
// Clears `out`; leaves it empty when the component is absent or keyframeless.
void bakeTracks(const AnimatedProperties* anim, std::vector<bus::BakedTrack>& out) {
    out.clear();
    if (!anim || !anim->hasAnyKeyframes()) return;
    out.reserve(anim->tracks.size());
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
        out.push_back(std::move(dst));
    }
}

// Re-evaluate baked animation tracks at localFrame and write the resulting
// transform matrix + opacity. Shared by the clip (applyBakedAnimation) and
// generative content-layer re-eval paths. outMatrix is overwritten only when
// a transform channel is animated; outOpacity is always set. No registry
// reads — safe on the show thread.
void evaluateBakedTransform(const std::vector<bus::BakedTrack>& tracks,
                            const std::array<float, 3>& basePos,
                            const std::array<float, 3>& baseRot,
                            const std::array<float, 3>& baseScale,
                            float baseOpacity,
                            FrameNumber localFrame,
                            std::array<float, 16>& outMatrix,
                            float& outOpacity,
                            const remote::RemoteControlStore::Sample* remoteSample = nullptr) {
    glm::vec3 position(basePos[0], basePos[1], basePos[2]);
    glm::vec3 rotation(baseRot[0], baseRot[1], baseRot[2]);
    glm::vec3 scale(baseScale[0], baseScale[1], baseScale[2]);
    float     opacity = baseOpacity;
    bool      transformDirty = false;

    for (const auto& track : tracks) {
        if (!track.enabled || track.keyframes.empty()) continue;
        const float v = evaluateBakedTrack(track, localFrame);
        switch (static_cast<AnimatableProperty>(track.property)) {
            case AnimatableProperty::PositionX: position.x = v; transformDirty = true; break;
            case AnimatableProperty::PositionY: position.y = v; transformDirty = true; break;
            case AnimatableProperty::Rotation:  rotation.z = v; transformDirty = true; break;
            case AnimatableProperty::ScaleX:    scale.x    = v; transformDirty = true; break;
            case AnimatableProperty::ScaleY:    scale.y    = v; transformDirty = true; break;
            case AnimatableProperty::Opacity:   opacity    = v;                         break;
            default: break;
        }
    }

    // ADR-0028: engaged remote values beat keyframes per axis. Applied AFTER
    // the track loop so remote always wins. Show thread only — indexed atomic
    // loads via Sample*, no locks, no registry.
    if (remoteSample && remoteSample->engaged) {
        using RP = remote::RemoteParam;
        if (remoteSample->has(RP::Opacity))
            opacity = std::clamp(remoteSample->get(RP::Opacity), 0.0f, 1.0f);
        if (remoteSample->has(RP::PosX))    { position.x = remoteSample->get(RP::PosX);    transformDirty = true; }
        if (remoteSample->has(RP::PosY))    { position.y = remoteSample->get(RP::PosY);    transformDirty = true; }
        if (remoteSample->has(RP::ScaleX))  { scale.x    = remoteSample->get(RP::ScaleX);  transformDirty = true; }
        if (remoteSample->has(RP::ScaleY))  { scale.y    = remoteSample->get(RP::ScaleY);  transformDirty = true; }
        if (remoteSample->has(RP::Rotation)){ rotation.z = remoteSample->get(RP::Rotation); transformDirty = true; }
    }

    if (transformDirty) {
        // Mirrors Transform::updateMatrix: Translation * Rot(Z*Y*X) * Scale.
        glm::mat4 m(1.0f);
        m = glm::translate(m, position);
        m = glm::rotate(m, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        m = glm::rotate(m, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        m = glm::rotate(m, glm::radians(rotation.x), glm::vec3(1, 0, 0));
        m = glm::scale(m, scale);
        std::memcpy(outMatrix.data(), glm::value_ptr(m), sizeof(outMatrix));
    }
    outOpacity = opacity;
}

// Apply animation tracks (and optional remote overlay) from the clip catalog
// to the render state. No registry reads — safe on the show thread.
void applyBakedAnimation(const bus::ClipCatalogEntry& ce,
                         FrameNumber currentFrame,
                         const remote::RemoteControlStore* store,
                         bus::ClipRenderState& out) {
    // Sample the remote store once (indexed atomic loads, no locks).
    remote::RemoteControlStore::Sample remoteSample;
    const remote::RemoteControlStore::Sample* remotePtr = nullptr;
    if (store && ce.remoteSlot >= 0) {
        remoteSample = store->sample(ce.remoteSlot);
        if (remoteSample.engaged && remoteSample.presentMask != 0)
            remotePtr = &remoteSample;
    }

    // Static clip with no remote override: catalog values are authoritative.
    if (ce.tracks.empty() && !remotePtr) return;

    // localFrame mirrors AnimationSystem::update: clip-local frame, clamped
    // to non-negative. If the clip isn't active at currentFrame we still
    // get sensible values (clamped to first/last keyframe).
    const FrameNumber localFrame =
        (currentFrame >= ce.startFrame) ? (currentFrame - ce.startFrame) : FrameNumber{0};

    evaluateBakedTransform(ce.tracks, ce.position, ce.rotation, ce.scale,
                           ce.opacity, localFrame,
                           out.transformMatrix, out.opacity, remotePtr);
}

} // namespace

PlaybackTimeAuthority::PlaybackTimeAuthority(entt::registry& registry, Timeline* timeline)
    : m_registry(registry)
    , m_timeline(timeline)
{}

PlaybackTimeAuthority::~PlaybackTimeAuthority() = default;

RateSource* PlaybackTimeAuthority::selectRateSource() {
    if (m_audioRate && m_audioRate->healthy()) return m_audioRate;
    return &m_systemRate;
}

void PlaybackTimeAuthority::startTiming() {
    RateSource* sel = selectRateSource();
    m_activeRate.store(sel, std::memory_order_release);
    m_lastSelected = sel;
    m_lastNow      = sel->now();
    m_startNow     = m_lastNow;
    m_deltaTime    = 0.0;
    m_elapsedTime  = 0.0;
    m_frameCount   = 0;
}

void PlaybackTimeAuthority::updateTiming() {
    RateSource* sel = selectRateSource();
    if (sel != m_lastSelected) {
        m_lastSelected = sel;
        m_lastNow      = sel->now();
        m_activeRate.store(sel, std::memory_order_release);
        m_deltaTime    = 0.0;
        return;
    }
    const double n = sel->now();
    m_deltaTime    = n - m_lastNow;
    if (m_deltaTime < 0.0) m_deltaTime = 0.0;
    m_elapsedTime += m_deltaTime;
    m_lastNow      = n;
}

FrameNumber PlaybackTimeAuthority::sectionFadeTailFrames(FrameNumber endFrame) const {
    if (!m_timeline) return 0;
    return timeline::sectionFadeTailFrames(*m_timeline, endFrame);
}

FrameNumber PlaybackTimeAuthority::computeExtendedDuration(const Clip& clip) const {
    if (!m_timeline) return clip.duration;
    const FrameNumber endFrame = clip.startFrame + clip.duration;
    const bool endAlignsWithBreak = sectionFadeTailFrames(endFrame) > 0;
    const double endingBreakFadeSeconds =
        timeline::sectionFadeSecondsAtBreak(*m_timeline, endFrame);
    const double timelineFps = m_timeline->getFrameRate();
    return entity::computeExtendedDuration(clip, timelineFps,
                                           endAlignsWithBreak,
                                           endingBreakFadeSeconds);
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

    // 2026-05-23 — Normal-mode break-aligned extension. A Normal-mode clip
    // whose end aligns with a section break (tailFrames > 0) extends its
    // active window past clip.duration up to the source out point. Locked
    // and non-break-aligned clips keep the original `endFrame + tail`
    // window. The natural wrap math in mapToMediaFrame walks through the
    // extension window naturally — Freeze/Loop/PingPong kicks in at the
    // source-out boundary as before. See ADR-0012 amendment 2026-05-23.
    if (clip.sectionBehavior == SectionBehavior::Normal && tailFrames > 0) {
        const FrameNumber extendedEnd = clip.startFrame + computeExtendedDuration(clip);
        if (extendedEnd > endFrame) {
            if (frame < extendedEnd) return true;
            // Section-fade tail attaches to the clip's true end. If
            // extendedEnd itself aligns with another break, the held +
            // fade window applies past extendedEnd; otherwise the clip
            // simply becomes inactive at extendedEnd.
            return frame < extendedEnd + sectionFadeTailFrames(extendedEnd);
        }
    }
    return frame < endFrame + tailFrames;
}

FrameNumber PlaybackTimeAuthority::mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    double frameRateRatio = clip.framerate / timelineFrameRate;

    FrameNumber sourceLength = effectivePlaybackLength(clip);

    // Phase 6 — post-end tail (held last decoded frame). isClipActiveAtFrame
    // gates entry into this window, so when timelineFrame >= realEnd we
    // know we're inside the fade tail. Don't advance past the source.
    //
    // Fix 5 (2026-05-23) — clamp timelineFrame to realEnd-1 and let
    // the wrap math below produce the correct held frame. The previous
    // `mediaStartFrame + sourceLength - 1` formula returned the trimmed
    // source range's end, which a clip with authored duration < trimmed
    // source range never decoded.
    //
    // 2026-05-23 amendment — realEnd is the Normal-mode break-aligned
    // extended end (== startFrame + duration for Locked / non-break-aligned
    // clips). Inside `[clipEnd, extendedEnd)` for an extended Normal clip
    // the timelineFrame is unclamped, so localFrame walks naturally past
    // clip.duration; the wrap math at sourceLocalFrame >= sourceLength
    // (Freeze/Loop/PingPong) kicks in at the source-out boundary.
    const FrameNumber realEnd = clip.startFrame + computeExtendedDuration(clip);
    if (timelineFrame >= realEnd) timelineFrame = realEnd - 1;

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
    // 2026-05-23 — realEnd is the Normal-mode break-aligned extended end
    // (== startFrame + duration for Locked / non-break-aligned clips). All
    // "past the play window?" checks below use realEnd so an extended
    // Normal clip plays naturally past clipEnd before tail-hold kicks in.
    // See ADR-0012 amendment 2026-05-23.
    const FrameNumber realEnd = clip.startFrame + computeExtendedDuration(clip);

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
    // 2-arg overload's last-decoded-frame clamp. For Normal-extended
    // clips this fires only past extendedEnd — within `[clipEnd,
    // extendedEnd)` natural playback / postBreakMediaAnchor takes over.
    if (timelineFrame >= realEnd && phase && phase->tailHoldMediaFrame >= 0) {
        return phase->tailHoldMediaFrame;
    }

    if (!phase || !phase->inContinuation
            || clip.sectionBehavior != SectionBehavior::Normal) {
        // Round-2 fixup, Phase 4 — post-break anchor. Spanning Normal-mode
        // clips that observed an at-break pause have a source-frame
        // anchor stamped by SectionScheduler::go(). The anchor branch
        // fires only after continuation has ended (post-GO,
        // !inContinuation) and only inside the clip's real play window
        // (timelineFrame < realEnd; the tail-hold short-circuit above
        // already handled timelineFrame >= realEnd). It maps mediaFrame
        // as `anchor + (timelineFrame - anchorTimelineFrame) * ratio`,
        // wrapped per playbackMode — same math as the natural-mapping
        // path but anchored to where the user was watching at GO.
        // Defensive guard (2026-05-23) — only apply the anchor when the
        // playhead is at or past the anchor's reference timeline frame.
        // See the parallel guard + rationale in
        // mapToMediaFrameFromCatalog.
        if (phase && phase->postBreakMediaAnchor >= 0
                && timelineFrame >= phase->anchorTimelineFrame
                && timelineFrame < realEnd) {
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

float PlaybackTimeAuthority::computeSectionFadeMultiplier(
    FrameNumber layerStart, FrameNumber layerEnd) const {
    if (!m_timeline) return 1.0f;
    auto [sections, rawFps] = m_timeline->copySectionsAndRate();
    if (sections.empty()) return 1.0f;

    const FrameNumber currentFrame = m_timeline->getCurrentFrame();
    const double timelineFrameRate = rawFps > 0.0 ? rawFps : 30.0;

    float multiplier = 1.0f;

    // At-break visibility gate. Layers that START at the current break stay
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
    //       break-aligned layer flashes at full opacity for 1-3 ticks
    //       (the "blip" bug). Excluding Paused here keeps scrub-to-break
    //       editing — where state is Paused after Timeline::seek and
    //       sectionAtBreak() is false — showing the layer's first frame.
    {
        const bool atBreakLatched = m_timeline->sectionAtBreak();
        const bool isPlaying =
            m_timeline->getPlaybackState() == PlaybackState::Playing;
        if (atBreakLatched || isPlaying) {
            for (const auto& sec : sections) {
                // breakFrame is an integer timeline frame — an exact match
                // on both the playhead and the layer's start gates it.
                if (sec.breakFrame == currentFrame &&
                    layerStart == sec.breakFrame) {
                    multiplier = 0.0f;
                    break;
                }
            }
        }
    }

    for (const auto& sec : sections) {
        if (sec.fadeSeconds <= 0.0) continue;
        const FrameNumber breakFrame = sec.breakFrame;
        const FrameNumber fadeFrames = static_cast<FrameNumber>(
            std::ceil(sec.fadeSeconds * timelineFrameRate));
        if (fadeFrames <= 0) continue;

        // Fade in: layer's start sits exactly on this break.
        if (breakFrame == layerStart) {
            if (currentFrame >= layerStart &&
                currentFrame < layerStart + fadeFrames) {
                const float t = static_cast<float>(currentFrame - layerStart)
                              / static_cast<float>(fadeFrames);
                // min-combine: a layer with both ends on breaks (shorter
                // than fadeIn + fadeOut) takes the more restrictive ramp.
                multiplier = std::min(multiplier, t);
            }
        }
        // Phase 6 — fade-out is the post-end held tail. Window is
        // [layerEnd, layerEnd + fadeFrames). At currentFrame == layerEnd
        // the multiplier is 1.0 (fully visible at the break, matches the
        // at-break held look), ramping linearly to 0.0 at the window's
        // open upper edge. The post-end window can't overlap the
        // at-start fade-in window of the *same* layer, so the min-combine
        // for both-aligned layers still works cleanly.
        if (breakFrame == layerEnd) {
            if (currentFrame >= layerEnd &&
                currentFrame < layerEnd + fadeFrames) {
                const float t = 1.0f - static_cast<float>(currentFrame - layerEnd)
                                      / static_cast<float>(fadeFrames);
                multiplier = std::min(multiplier, t);
            }
        }
    }
    return std::clamp(multiplier, 0.0f, 1.0f);
}

float PlaybackTimeAuthority::computeSectionFadeMultiplier(const Clip& clip) const {
    return computeSectionFadeMultiplier(clip.startFrame,
                                        clip.startFrame + clip.duration);
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
    // Stage primitives baked here: Screens, OutputSurfaces, Projectors,
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

        // Fold in ObjectAnimationOutput from active OA layers targeting this
        // screen. Precedence per ADR-0020: active layer beats after-end-Hold
        // layer; within either category, lower trackIndex wins.
        const ObjectAnimationOutput* winningActiveOA = nullptr;
        uint32_t winningActiveIdx = UINT32_MAX;
        const ObjectAnimationOutput* winningHoldOA = nullptr;
        uint32_t winningHoldIdx = UINT32_MAX;
        for (auto [oaEntity, oal, oaOut, oaLayer] : oaScan.each()) {
            if (oal.target != entity) continue;
            const bool active   = currentFrameForOA >= oaLayer.startFrame &&
                                  currentFrameForOA <  oaLayer.startFrame + oaLayer.duration;
            const bool afterEnd = currentFrameForOA >= oaLayer.startFrame + oaLayer.duration;
            if (active) {
                if (oaLayer.trackIndex < winningActiveIdx) {
                    winningActiveIdx = oaLayer.trackIndex;
                    winningActiveOA  = &oaOut;
                }
            } else if (afterEnd &&
                       oal.endBehavior == ObjectAnimationLayer::EndBehavior::Hold) {
                if (oaLayer.trackIndex < winningHoldIdx) {
                    winningHoldIdx = oaLayer.trackIndex;
                    winningHoldOA  = &oaOut;
                }
            }
        }
        const ObjectAnimationOutput* winningOA =
            winningActiveOA ? winningActiveOA : winningHoldOA;
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
    for (auto [entity, surf] : m_registry.view<OutputSurface>().each()) {
        bus::OutputSurfaceSnapshot ms;
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
        // #74 — carried for effectivePlaybackLength's fallback when
        // mediaOutFrame is the unresolved -1 sentinel (missing media).
        ce.totalMediaFrames = clip.totalMediaFrames;
        ce.framerate       = clip.framerate;
        ce.playbackMode    = static_cast<int>(clip.playbackMode);
        ce.sectionBehavior = static_cast<int>(clip.sectionBehavior);
        // 2026-05-23 — bake whether the clip's end aligns with a section
        // break so the show side can compute the Normal-mode extended
        // duration without reading live Timeline section state. The bake
        // happens once per editor frame here; the show side reads it
        // verbatim from the catalog. See ADR-0012 amendment 2026-05-23.
        const FrameNumber clipEndFrame = clip.startFrame + clip.duration;
        ce.endAlignsWithSectionBreak =
            sectionFadeTailFrames(clipEndFrame) > 0;
        // 2026-05-23 follow-up — bake the fadeSeconds of the section
        // break at the clip's end (0.0 when no aligned break or break
        // has no fade). Gates the Normal-mode extension on
        // fadeSeconds == 0 so the decoder stops at the fade window end
        // instead of running to source-out. Fixes the two-clip-meeting-
        // at-fade-break cache thrash. See Message.hpp.
        ce.endingBreakFadeSeconds =
            m_timeline
                ? timeline::sectionFadeSecondsAtBreak(*m_timeline, clipEndFrame)
                : 0.0;
        ce.descriptorSlot  = static_cast<int>(videoTex.descriptorSlot);
        // #90: carry the slot's reuse generation only for a real slot. The
        // VideoTexture unallocated sentinel is UINT32_MAX (which the cast above
        // turns into descriptorSlot == -1); 0 there matches the field default.
        ce.descriptorGeneration =
            (videoTex.descriptorSlot != UINT32_MAX) ? videoTex.generation : 0u;

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
        // during editor stalls (NEW-07). Empty for clips without animation;
        // the show side then uses the static ce.transformMatrix / ce.opacity.
        bakeTracks(m_registry.try_get<AnimatedProperties>(entity), ce.tracks);

        // ADR-0028: bake the RemoteControlStore slot so the show thread can
        // sample it with indexed atomic loads (no registry access show-side).
        if (const auto* rp = m_registry.try_get<RemotePatch>(entity))
            ce.remoteSlot = rp->storeSlot;

        ce.targetScreen = resolveTargetScreen(m_registry, entity, clip.targetScreen);
        bakeRoutes(m_registry, entity, ce.routes, ce.mode);

        ce.ocioOverride = lookupInputColorSpaceOverride(clip);

        if (const auto* phase = m_registry.try_get<ClipPlaybackPhase>(entity)) {
            ce.hasPhase                  = true;
            ce.phase_inContinuation      = phase->inContinuation;
            ce.phase_sourcePhaseFrames   = phase->sourcePhaseFrames;
            ce.phase_tailHoldMediaFrame  = phase->tailHoldMediaFrame;
            ce.phase_postBreakMediaAnchor = phase->postBreakMediaAnchor;
            ce.phase_anchorTimelineFrame  = phase->anchorTimelineFrame;
            // NEW-08 wall-clock anchor — lets the show thread re-derive the
            // live continuation phase during editor stalls.
            ce.phase_continuationStartTimeNs = phase->continuationStartTimeNs;
            ce.phase_continuationSeedFrames  = phase->continuationSeedFrames;
        }

        out.clipCatalog.push_back(std::move(ce));
    }

    // OA layer catalog — all OA layers with AnimatedProperties baked into the
    // snapshot so the show thread can re-evaluate tracks per render frame and
    // fold results into ScreenSnapshot position/rotation/scale during editor
    // stalls (mirrors the NEW-07 clip-animation fix via applyBakedAnimation).
    // Layers without keyframes are omitted — the static fold-in above via
    // ObjectAnimationOutput is sufficient for those.
    //
    // ADR-0020: layers in their after-end window with EndBehavior::Hold also
    // ride the bus so the show thread can keep holding the last-evaluated
    // values during editor stalls. The Reset case is a no-op (output cleared
    // editor-side) and stays off the bus.
    out.objectAnimationLayers.clear();
    for (auto [oaEntity, oal, oaLayer] : m_registry.view<ObjectAnimationLayer, Layer>().each()) {
        const bool isActive = currentFrameForOA >= oaLayer.startFrame &&
                              currentFrameForOA <  oaLayer.startFrame + oaLayer.duration;
        const bool isAfterEnd = currentFrameForOA >= oaLayer.startFrame + oaLayer.duration;
        const bool holdAfterEnd = isAfterEnd &&
            oal.endBehavior == ObjectAnimationLayer::EndBehavior::Hold;
        if (!isActive && !holdAfterEnd) continue;

        const auto* anim = m_registry.try_get<AnimatedProperties>(oaEntity);
        if (!anim || !anim->hasAnyKeyframes()) continue;

        bus::ObjectAnimationLayerSnapshot oas;
        oas.targetScreen = (oal.target == entt::null)
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(oal.target);
        oas.startFrame  = oaLayer.startFrame;
        oas.duration    = oaLayer.duration;
        oas.trackIndex  = oaLayer.trackIndex;
        oas.endBehavior = (oal.endBehavior == ObjectAnimationLayer::EndBehavior::Reset)
            ? bus::OAEndBehavior::Reset
            : bus::OAEndBehavior::Hold;

        bakeTracks(anim, oas.tracks);
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
        // Active window extended by sectionFadeTailFrames the same way
        // isClipActiveAtFrame does for Clips. With Fix 1 this returns >= 1
        // for any layer whose end aligns with a break, so trailing-edge
        // generatives stay in the snapshot during the at-break park (and
        // through any post-end fade-out tail). Without this a Text or
        // Muncher layer ending on a break would drop the moment the
        // playhead reached the break — same bug as Fix 1, same fix.
        const FrameNumber layerEnd   = layer.startFrame + layer.duration;
        const FrameNumber tailFrames = sectionFadeTailFrames(layerEnd);
        if (currentFrameForOA < layer.startFrame ||
            currentFrameForOA >= layerEnd + tailFrames) continue;

        bus::GenerativeLayerSnapshot snap;
        snap.entity       = static_cast<std::uint64_t>(entity);
        snap.targetScreen = resolveTargetScreen(m_registry, entity, gen.targetScreen);
        bakeRoutes(m_registry, entity, snap.routes, snap.mode);
        snap.startFrame   = layer.startFrame;
        snap.duration     = layer.duration;

        if (const auto* ml = m_registry.try_get<MediaLayer>(entity)) {
            snap.opacity   = ml->opacity;
            snap.blendMode = static_cast<int>(ml->blendMode);
            snap.zOrder    = ml->zOrder;
        }

        // UV-space transform — same bake pattern as ClipCatalogEntry. Also
        // bake the individual axes so the show thread can re-evaluate
        // animation tracks per render frame (NEW-07).
        if (auto* t = m_registry.try_get<Transform>(entity)) {
            t->updateMatrix();
            std::memcpy(snap.transformMatrix.data(),
                        glm::value_ptr(t->matrix),
                        sizeof(snap.transformMatrix));
            snap.position = { t->position.x, t->position.y, t->position.z };
            snap.rotation = { t->rotation.x, t->rotation.y, t->rotation.z };
            snap.scale    = { t->scale.x,    t->scale.y,    t->scale.z    };
        }

        // AnimatedProperties → BakedTrack snapshot for stall-safe show-thread
        // re-evaluation (NEW-07). Empty for generative layers without keyframes.
        bakeTracks(m_registry.try_get<AnimatedProperties>(entity), snap.tracks);

        // ADR-0028: bake the RemoteControlStore slot for show-side overlay.
        if (const auto* rp = m_registry.try_get<RemotePatch>(entity))
            snap.remoteSlot = rp->storeSlot;

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
        } else if (const auto* tls = m_registry.try_get<TextLayerState>(entity)) {
            snap.kind            = bus::GenerativeLayerSnapshot::Kind::Text;
            snap.textTextureSlot = tls->textureSlot;
            snap.textBakedWidth  = tls->bakedWidth;
            snap.textBakedHeight = tls->bakedHeight;
        } else {
            // No kind-specific state component → skip (defensive — the
            // editor-side creation paths always attach one).
            continue;
        }

        out.generativeLayers.push_back(std::move(snap));
    }

    // Signal Output Layer catalog — baked for SignalOutputSystem (Phase 5/6).
    // Momentary: active for the single frame equal to startFrame so the show
    // thread can detect the playhead crossing into the layer. Continuous:
    // active while currentFrameForOA is in [startFrame, startFrame+duration).
    out.signalLayers.clear();
    for (auto [sigEntity, sigLayer, sigLay] :
            m_registry.view<SignalLayer, Layer>().each()) {
        const FrameNumber layStart = sigLay.startFrame;
        const FrameNumber layEnd   = layStart + sigLay.duration;
        const bool isMomentary = (sigLayer.mode == SignalLayer::Mode::Momentary);

        bool active = false;
        if (isMomentary) {
            // ±2-frame straddling window around startFrame. The headless loop
            // runs at thousands of fps and cue-jumps advance the playhead by
            // many frames in one tick, so exact equality (== layStart) silently
            // drops cues whenever the tick steps over startFrame. The ±2-frame
            // window ensures the layer appears in the snapshot for prev=start-1
            // → cur=start+2 skips. Phase 5's crossing detector re-arms once the
            // layer exits the window so it still fires exactly once per crossing.
            active = (currentFrameForOA >= layStart - 2 &&
                      currentFrameForOA <= layEnd   + 2);
        } else {
            // Continuous: any frame inside [start, end).
            active = (currentFrameForOA >= layStart && currentFrameForOA < layEnd);
        }
        if (!active) continue;

        bus::SignalLayerSnapshot sl;
        sl.entity     = static_cast<std::uint64_t>(sigEntity);
        sl.startFrame = sigLay.startFrame;
        sl.duration   = sigLay.duration;
        sl.mode       = static_cast<int>(sigLayer.mode);
        sl.transport  = static_cast<int>(sigLayer.transport);
        sl.valueSource = static_cast<int>(sigLayer.valueSource);
        sl.address    = sigLayer.address;
        sl.srcMin     = sigLayer.srcMin;
        sl.srcMax     = sigLayer.srcMax;
        sl.outMin     = sigLayer.outMin;
        sl.outMax     = sigLayer.outMax;
        sl.outType    = static_cast<int>(sigLayer.outType);

        // Bake args: translate SignalLayer::Arg -> bus::SignalArg.
        // Track the value-bound arg index for SignalOutputSystem.
        sl.valueBoundArgIndex = -1;
        for (std::size_t ai = 0; ai < sigLayer.args.size(); ++ai) {
            const auto& src = sigLayer.args[ai];
            bus::SignalArg dst;
            // SignalLayer::Arg::Type matches bus::SignalArg::Type by value (0/1/2).
            dst.type = static_cast<bus::SignalArg::Type>(static_cast<int>(src.type));
            dst.i    = src.i;
            dst.f    = src.f;
            dst.s    = src.s;
            sl.args.push_back(dst);
            if (src.isValueBound && sl.valueBoundArgIndex < 0)
                sl.valueBoundArgIndex = static_cast<int>(ai);
        }

        // Bake AnimatedProperties tracks (NEW-07 pattern) so the show thread
        // can re-evaluate SignalValue keyframes during editor stalls.
        bakeTracks(m_registry.try_get<AnimatedProperties>(sigEntity), sl.tracks);

        out.signalLayers.push_back(std::move(sl));
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
                        const float v = evaluateKeyframes(
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

    // Snapshot the active rate-source time once per render frame and share it
    // across all catalog entries so every clip's continuation re-derivation
    // runs against the same instant (audio crystal when AudioRateSource is active).
    const std::int64_t rateNowNs = static_cast<std::int64_t>(rateNow() * 1e9);

    // Show thread reads ONLY the clip catalog — zero registry access.
    for (const auto& ce : scene.clipCatalog) {
        const Clip clip = clipFromCatalog(ce);
        if (!isClipActiveAtFrame(clip, currentFrame)) continue;

        bus::ClipRenderState c;
        c.entity       = ce.entity;
        c.slot         = ce.descriptorSlot;
        c.slotGeneration = ce.descriptorGeneration;  // #90
        c.mediaFrame   = mapToMediaFrameFromCatalog(ce, currentFrame, timelineFrameRate, rateNowNs);
        c.ocioOverride = ce.ocioOverride;
        c.transformMatrix = ce.transformMatrix;
        c.opacity      = ce.opacity;
        c.blendMode    = static_cast<BlendMode>(ce.blendMode);
        c.zOrder       = ce.zOrder;
        c.targetScreen = ce.targetScreen;
        c.sectionFadeMultiplier = computeSectionFadeMultiplier(clip);

        // NEW-07 + ADR-0028: re-evaluate animation tracks at the show thread's
        // current frame; apply engaged remote overlay on top. No-op for static
        // clips with no remote engagement.
        applyBakedAnimation(ce, currentFrame, m_remoteStore, c);

        out.activeClips.push_back(std::move(c));
    }

    // OA stall-safety: re-evaluate baked OA tracks per render frame and fold
    // results into the ScreenSnapshot entries in out.screens. This mirrors
    // what buildSceneSnapshot does on the editor side via ObjectAnimationOutput,
    // but runs on the show thread so animation survives editor stalls.
    //
    // Priority within a target screen (ADR-0020):
    //   1. Active layers (currentFrame inside [startFrame, startFrame+duration))
    //      beat after-end-Hold layers. Among multiple active layers, lower
    //      trackIndex wins.
    //   2. If no active layer targets this screen, an after-end-Hold layer wins
    //      (KeyframeTrack::evaluate clamps to last keyframe value beyond
    //      duration). Lower trackIndex still wins among multiple.
    //   3. Reset-mode layers never appear in scene.objectAnimationLayers after
    //      their active window — they're filtered out editor-side in
    //      buildSceneSnapshot.
    for (auto& ss : out.screens) {
        const bus::ObjectAnimationLayerSnapshot* winningActive    = nullptr;
        const bus::ObjectAnimationLayerSnapshot* winningHoldAfter = nullptr;
        for (const auto& oa : scene.objectAnimationLayers) {
            if (oa.targetScreen != ss.entity) continue;
            const bool active   = currentFrame >= oa.startFrame &&
                                  currentFrame <  oa.startFrame + oa.duration;
            const bool afterEnd = currentFrame >= oa.startFrame + oa.duration;
            if (active) {
                if (!winningActive || oa.trackIndex < winningActive->trackIndex)
                    winningActive = &oa;
            } else if (afterEnd && oa.endBehavior == bus::OAEndBehavior::Hold) {
                if (!winningHoldAfter || oa.trackIndex < winningHoldAfter->trackIndex)
                    winningHoldAfter = &oa;
            }
        }
        const bus::ObjectAnimationLayerSnapshot* winning =
            winningActive ? winningActive : winningHoldAfter;
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
                case AnimatableProperty::RotationZ: ss.rotation[2] = v; break;
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

    // Build the route list for one content layer. ADR-0021 M3: prefer the
    // editor-baked `bakedRoutes` (carried on ClipCatalogEntry / GenerativeLayer
    // Snapshot) when non-empty — that's the canonical Tiled multi-target case
    // and any Direct single-target Plane A authoring. When the bake produced
    // no routes (legacy clip without ContentRouting or empty-targets
    // ContentRouting), fall back to deriving a route from the single-screen
    // `targetScreen` alias; UINT64_MAX there expands to one route per visible
    // screen so PASS 2 sees an explicit destination list with no implicit
    // "all visible" case. When `out.screens` is empty (mid-project-load,
    // no Screens authored yet), an unbound UINT64_MAX route is emitted so
    // the snapshot stays self-describing.
    auto fillRoutes = [&](bus::ContentLayerSnapshot& c,
                           const std::vector<bus::ContentLayerRoute>& bakedRoutes,
                           int bakedMode,
                           std::uint64_t targetScreen) {
        c.routes.clear();
        if (!bakedRoutes.empty()) {
            // Editor-baked Plane A path. Pass through the route list as-is;
            // UINT64_MAX in a baked route still expands to per-visible-screen
            // below to keep PASS 2 explicit-list semantics.
            c.mode = bakedMode;
            for (const auto& br : bakedRoutes) {
                if (br.screen == UINT64_MAX) {
                    if (out.screens.empty()) {
                        c.routes.push_back(br);  // no screens yet — keep sentinel
                    } else {
                        for (const auto& s : out.screens) {
                            if (!s.visible) continue;
                            bus::ContentLayerRoute r = br;
                            r.screen = s.entity;
                            c.routes.push_back(r);
                        }
                    }
                } else {
                    c.routes.push_back(br);
                }
            }
            if (c.routes.empty()) {
                bus::ContentLayerRoute r;
                r.screen = UINT64_MAX;
                c.routes.push_back(r);
            }
            return;
        }
        c.mode = 0;  // legacy fallback is always Direct
        if (targetScreen == UINT64_MAX) {
            if (out.screens.empty()) {
                bus::ContentLayerRoute r;
                r.screen = UINT64_MAX;
                c.routes.push_back(r);
            } else {
                c.routes.reserve(out.screens.size());
                for (const auto& s : out.screens) {
                    if (!s.visible) continue;
                    bus::ContentLayerRoute r;
                    r.screen = s.entity;
                    c.routes.push_back(r);
                }
                if (c.routes.empty()) {
                    bus::ContentLayerRoute r;
                    r.screen = UINT64_MAX;
                    c.routes.push_back(r);
                }
            }
        } else {
            bus::ContentLayerRoute r;
            r.screen = targetScreen;
            c.routes.push_back(r);
        }
    };

    // Catalog lookup for clip's editor-baked routes (linear scan, N tiny).
    auto findCatalog = [&scene](std::uint64_t entity) -> const bus::ClipCatalogEntry* {
        for (const auto& ce : scene.clipCatalog) {
            if (ce.entity == entity) return &ce;
        }
        return nullptr;
    };

    for (const auto& ac : out.activeClips) {
        bus::ContentLayerSnapshot c;
        c.entity                = ac.entity;
        c.targetScreen          = ac.targetScreen;
        const auto* ce = findCatalog(ac.entity);
        if (ce) {
            fillRoutes(c, ce->routes, ce->mode, ac.targetScreen);
        } else {
            static const std::vector<bus::ContentLayerRoute> kEmpty;
            fillRoutes(c, kEmpty, 0, ac.targetScreen);
        }
        c.transformMatrix       = ac.transformMatrix;
        c.opacity               = ac.opacity;
        c.sectionFadeMultiplier = ac.sectionFadeMultiplier;
        c.blendMode             = static_cast<int>(ac.blendMode);
        c.zOrder                = ac.zOrder;
        c.sourceKind            = bus::ContentLayerSnapshot::SourceKind::Video;
        c.sourceSlot            = ac.slot;
        c.sourceGeneration      = ac.slotGeneration;  // #90
        // Carry remoteSlot for diagnostics / future per-entry overlay.
        if (ce) c.remoteSlot   = ce->remoteSlot;
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

    // Live re-filter, mirroring the isClipActiveAtFrame re-check on the clip
    // path above. Membership baked in buildSceneSnapshot is stale by the
    // editor's bake lag; a break-aligned generative/text layer whose fade-out
    // has completed must leave the draw list on the SAME live frame the fade
    // window closes — past the window computeSectionFadeMultiplier returns
    // 1.0, so a lingering member draws at full opacity (the "pop"). The
    // lower/upper bounds match the editor bake at buildSceneSnapshot exactly:
    // [startFrame, layerEnd + sectionFadeTailFrames(layerEnd)).
    std::erase_if(out.generativeLayers, [&](const bus::GenerativeLayerSnapshot& gl) {
        const FrameNumber layerEnd = gl.startFrame + gl.duration;
        return currentFrame < gl.startFrame ||
               currentFrame >= layerEnd + sectionFadeTailFrames(layerEnd);
    });

    for (const auto& gl : out.generativeLayers) {
        bus::ContentLayerSnapshot c;
        c.entity                = gl.entity;
        c.targetScreen          = gl.targetScreen;
        fillRoutes(c, gl.routes, gl.mode, gl.targetScreen);
        c.transformMatrix       = gl.transformMatrix;
        c.opacity               = gl.opacity;
        // NEW-07 + ADR-0028: re-evaluate baked animation and apply remote
        // overlay. Always runs when either tracks or an engaged remote is
        // present; no-op for a fully static, unpatched generative layer.
        {
            remote::RemoteControlStore::Sample remoteSample;
            const remote::RemoteControlStore::Sample* remotePtr = nullptr;
            if (m_remoteStore && gl.remoteSlot >= 0) {
                remoteSample = m_remoteStore->sample(gl.remoteSlot);
                if (remoteSample.engaged && remoteSample.presentMask != 0)
                    remotePtr = &remoteSample;
            }
            if (!gl.tracks.empty() || remotePtr) {
                const FrameNumber localFrame =
                    (currentFrame >= gl.startFrame)
                        ? (currentFrame - gl.startFrame)
                        : FrameNumber{0};
                evaluateBakedTransform(gl.tracks, gl.position, gl.rotation, gl.scale,
                                       gl.opacity, localFrame,
                                       c.transformMatrix, c.opacity, remotePtr);
            }
        }
        // Bake section fade for generative layers — same envelope clips get,
        // via the layer-agnostic overload. Without this Text / Muncher layers
        // queued at a section break pop on at full opacity instead of waiting
        // invisible until GO + fading in.
        c.sectionFadeMultiplier = computeSectionFadeMultiplier(
            gl.startFrame, gl.startFrame + gl.duration);
        c.blendMode             = gl.blendMode;
        c.zOrder                = gl.zOrder;
        c.sourceKind            = bus::ContentLayerSnapshot::SourceKind::Compose;
        c.sourceSlot            = gl.renderTargetSlot;
        c.remoteSlot            = gl.remoteSlot;
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
