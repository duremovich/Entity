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

bool PlaybackTimeAuthority::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    return frame >= clip.startFrame && frame < (clip.startFrame + clip.duration);
}

FrameNumber PlaybackTimeAuthority::mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
    FrameNumber localFrame = timelineFrame - clip.startFrame;

    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    double frameRateRatio = clip.framerate / timelineFrameRate;
    FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

    FrameNumber sourceLength = clip.totalMediaFrames > 0 ? clip.totalMediaFrames :
        static_cast<FrameNumber>(clip.duration * frameRateRatio);

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
    // Continuation-phase override only applies when the component exists
    // AND the scheduler has placed the clip in continuation. Otherwise
    // fall through to the timeline-derived path so existing semantics
    // (and unit-test expectations) are preserved.
    const ClipPlaybackPhase* phase = m_registry.try_get<ClipPlaybackPhase>(entity);
    if (!phase || !phase->inContinuation) {
        return mapToMediaFrame(clip, timelineFrame);
    }

    // Continuation path: derive the media frame from the accumulated source
    // phase directly. Mirrors the Freeze/Loop/PingPong branches of the
    // timeline-derived path, but operates on `sourcePhaseFrames` instead
    // of the timeline-delta * frame-rate-ratio.
    const double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    const double frameRateRatio = (timelineFrameRate > 0.0)
        ? clip.framerate / timelineFrameRate : 1.0;
    const FrameNumber sourceLength = clip.totalMediaFrames > 0
        ? clip.totalMediaFrames
        : static_cast<FrameNumber>(clip.duration * frameRateRatio);

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
        // Fade out: clip's end coincides with this break.
        if (absDiff(breakFrame, clipEnd) <= snapTol) {
            if (clipEnd >= fadeFrames &&
                currentFrame > clipEnd - fadeFrames &&
                currentFrame <= clipEnd) {
                const float t = static_cast<float>(clipEnd - currentFrame)
                              / static_cast<float>(fadeFrames);
                multiplier = std::min(multiplier, t);
            }
        }
    }
    return std::clamp(multiplier, 0.0f, 1.0f);
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
