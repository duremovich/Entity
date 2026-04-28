#include "entity/director/PlaybackTimeAuthority.hpp"

#include "entity/components/Clip.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/timeline/Timeline.hpp"

#include <cmath>
#include <utility>

namespace entity {

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

std::string PlaybackTimeAuthority::lookupInputColorSpaceOverride(const Clip& clip) const {
    if (!m_projectManager) return {};
    if (clip.filepath.empty()) return {};
    const auto* entry = m_projectManager->findEntry(clip.filepath);
    if (!entry) return {};
    return entry->inputColorSpaceOverride;
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
        ac.mediaFrame     = mapToMediaFrame(clip, currentFrame);
        ac.ocioOverride   = lookupInputColorSpaceOverride(clip);
        out.push_back(std::move(ac));
    }
}

} // namespace entity
