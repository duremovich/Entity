#include "entity/director/PlaybackTimeAuthority.hpp"

#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/timeline/Timeline.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
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

void PlaybackTimeAuthority::buildRenderFrame(bus::RenderFrame& out) const {
    out.activeClips.clear();
    out.wantedFrames.clear();
    out.frameNumber = 0;
    out.deltaTime   = m_deltaTime;
    out.playState   = TransportState::Stopped;
    if (!m_timeline) return;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    out.frameNumber = currentFrame;
    out.playState   = toTransportState(m_timeline->getPlaybackState());

    auto view = m_registry.view<Clip, VideoTexture>();
    for (auto [entity, clip, videoTex] : view.each()) {
        if (!videoTex.isAllocated()) continue;
        if (!isClipActiveAtFrame(clip, currentFrame)) continue;

        bus::ClipRenderState c;
        c.entity       = static_cast<std::uint64_t>(entity);
        c.slot         = static_cast<int>(videoTex.descriptorSlot);
        c.mediaFrame   = mapToMediaFrame(entity, clip, currentFrame);
        c.ocioOverride = lookupInputColorSpaceOverride(clip);

        // Optional render-side fields. These exist so the wire format
        // already carries everything Phase E will need; the compositor
        // still reads from the registry today, so leaving them at
        // defaults would compile too -- filling them just keeps the
        // message a faithful snapshot of what the compositor would draw.
        if (auto* t = m_registry.try_get<Transform>(entity)) {
            // Transform::updateMatrix maintains a dirty-flag cache; mutate
            // through a non-const ref so the cached matrix is current.
            auto* tm = const_cast<Transform*>(t);
            tm->updateMatrix();
            std::memcpy(c.transformMatrix.data(),
                        glm::value_ptr(tm->matrix),
                        sizeof(c.transformMatrix));
        }
        if (auto* layer = m_registry.try_get<MediaLayer>(entity)) {
            c.opacity   = layer->getOpacity();
            c.blendMode = layer->blendMode;
        }
        c.targetScreen = (clip.targetScreen == entt::null)
            ? UINT64_MAX
            : static_cast<std::uint64_t>(clip.targetScreen);
        // Phase D — auto-fade envelope at section break boundaries.
        // Stamped here on the bus payload; PlaybackPresenter applies
        // it to MediaLayer.opacity so the registry-side compositor draw
        // reflects the fade this same tick.
        c.sectionFadeMultiplier = computeSectionFadeMultiplier(clip);

        out.activeClips.push_back(std::move(c));
    }
}

} // namespace entity
