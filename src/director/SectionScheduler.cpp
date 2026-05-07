#include "entity/director/SectionScheduler.hpp"

#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/timeline/Timeline.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace entity {

SectionScheduler::SectionScheduler(entt::registry& registry, Timeline* timeline)
    : m_registry(registry)
    , m_timeline(timeline)
{}

void SectionScheduler::tick(double deltaTimeSeconds) {
    if (!m_timeline) {
        m_atBreak = false;
        m_haveLastTickFrame = false;
        return;
    }

    const PlaybackState state = m_timeline->getPlaybackState();
    const Timecode currentTime = m_timeline->getCurrentTime();

    if (state == PlaybackState::Stopped) {
        m_atBreak = false;
        m_haveLastTickFrame = false;
        m_timeline->setSectionAtBreak(false);
        m_lastTickFrame = currentTime;
        // Stop also cancels continuation -- the clips would resume from
        // wherever the timeline next plays from, not from the parked phase.
        clearAllContinuation();
        return;
    }

    if (!m_haveLastTickFrame) {
        m_lastTickFrame = currentTime;
        m_haveLastTickFrame = true;
        return;
    }

    if (state != PlaybackState::Playing) {
        // Paused (manually or by us). While at-break, advance Normal-clip
        // continuation phase so Loop / PingPong clips keep cycling. If the
        // user just scrubbed elsewhere we'll detect a discontinuity below.
        if (m_atBreak) {
            advanceContinuation(deltaTimeSeconds);
        }
        m_lastTickFrame = currentTime;
        return;
    }

    // Manual-resume unstick: if the timeline transitioned to Playing while
    // we still hold the at-break latch (user clicked the UI play button or
    // a script fired Play / TogglePlayPause without going through GO), the
    // latch is stale. Drop it before processing this tick — otherwise the
    // next spacebar would fire SectionGo and yank the playhead back to the
    // last break.
    if (m_atBreak && currentTime > m_lastBreakHitFrame) {
        m_atBreak = false;
        m_timeline->setSectionAtBreak(false);
        clearAllContinuation();
    }

    const double frameRate = m_timeline->getFrameRate() > 0.0 ? m_timeline->getFrameRate() : 30.0;
    const Timecode oneFrame = static_cast<Timecode>(1000000.0 / frameRate);

    // Scrub jumps are not crossings — only continuous playback advances
    // trigger break detection. The threshold is "more than 2 timeline
    // frames OR more than 5× the per-tick advance, whichever is greater".
    // Without the deltaTime floor, high-fps timelines (>120 fps) put 2×
    // oneFrame below the per-render-tick advance (~16ms at 60Hz) and every
    // normal advance reads as a scrub.
    const Timecode delta = currentTime > m_lastTickFrame
                         ? currentTime - m_lastTickFrame
                         : m_lastTickFrame - currentTime;
    const Timecode tickAdvance = deltaTimeSeconds > 0.0
        ? static_cast<Timecode>(deltaTimeSeconds * 1000000.0)
        : static_cast<Timecode>(0);
    const Timecode discontinuityThreshold = std::max(oneFrame * 2, tickAdvance * 5);
    if (delta > discontinuityThreshold) {
        m_lastTickFrame = currentTime;
        return;
    }

    // Look for the first break strictly after the previous tick's frame.
    // If it sits at or before the current frame, the playhead crossed it.
    if (const Timeline::Section* brk = m_timeline->findNextBreakAfter(m_lastTickFrame)) {
        if (brk->breakFrame <= currentTime) {
            const Timecode hit = brk->breakFrame;
            m_timeline->seek(hit);
            // Timeline::seek auto-pauses if playing, but be explicit.
            m_timeline->pause();
            m_atBreak = true;
            m_lastBreakHitFrame = hit;
            m_lastTickFrame = hit;
            m_timeline->setSectionAtBreak(true);
            // Phase C: seed Normal clips with the source-frame phase they
            // had at the break so their continuation picks up smoothly.
            seedContinuationAt(hit);
            std::cout << "[SectionScheduler] At break frame=" << hit
                      << " ('" << brk->name << "')" << std::endl;
            return;
        }
    }

    m_lastTickFrame = currentTime;
}

void SectionScheduler::go() {
    if (!m_timeline || !m_atBreak) return;

    const double frameRate = m_timeline->getFrameRate() > 0.0 ? m_timeline->getFrameRate() : 30.0;
    const Timecode oneFrame = static_cast<Timecode>(1000000.0 / frameRate);
    const Timecode resumeFrame = m_lastBreakHitFrame + oneFrame;

    // Clear continuation BEFORE flipping the at-break flag so the very next
    // mapToMediaFrame call (post-resume) takes the timeline-derived path.
    clearAllContinuation();

    m_atBreak = false;
    m_timeline->setSectionAtBreak(false);
    m_timeline->seek(resumeFrame);
    m_timeline->play();
    m_lastTickFrame = resumeFrame;
    m_haveLastTickFrame = true;

    std::cout << "[SectionScheduler] GO from break " << m_lastBreakHitFrame
              << " -> resume at " << resumeFrame << std::endl;
}

void SectionScheduler::seedContinuationAt(Timecode breakFrameTime) {
    if (!m_timeline) return;
    const double timelineFrameRate = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;

    // Convert the break point from microseconds to timeline frames.
    const FrameNumber breakTimelineFrame = static_cast<FrameNumber>(
        (static_cast<double>(breakFrameTime) * timelineFrameRate) / 1000000.0);

    auto view = m_registry.view<Clip>();
    for (auto entity : view) {
        const Clip& clip = view.get<Clip>(entity);

        // Only Normal-mode clips that are active at the break get continuation.
        if (clip.sectionBehavior != SectionBehavior::Normal) continue;
        // Symmetric with advanceContinuation's guard. A clip with framerate <= 0
        // (uninitialized / corrupt project) would compute a zero phase and
        // silently land at mediaStartFrame regardless of where the break sat.
        if (clip.framerate <= 0.0) continue;
        if (breakTimelineFrame < clip.startFrame ||
            breakTimelineFrame >= clip.startFrame + clip.duration) {
            continue;
        }

        // Source-frame phase at the break = (timeline delta since clip
        // start) * (clipFps / timelineFps). Lazy-allocate the component
        // (existing components are reused — clearAllContinuation keeps
        // them around between break crossings).
        ClipPlaybackPhase& phase = m_registry.get_or_emplace<ClipPlaybackPhase>(entity);
        const double deltaTimelineFrames =
            static_cast<double>(breakTimelineFrame - clip.startFrame);
        const double frameRateRatio = clip.framerate / timelineFrameRate;
        phase.sourcePhaseFrames = deltaTimelineFrames * frameRateRatio;
        phase.inContinuation = true;
    }
}

void SectionScheduler::advanceContinuation(double deltaTimeSeconds) {
    if (deltaTimeSeconds <= 0.0) return;
    auto view = m_registry.view<Clip, ClipPlaybackPhase>();
    for (auto entity : view) {
        const Clip& clip = view.get<Clip>(entity);
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);
        if (!phase.inContinuation) continue;
        if (clip.sectionBehavior != SectionBehavior::Normal) continue;
        if (clip.framerate <= 0.0) continue;
        phase.sourcePhaseFrames += deltaTimeSeconds * clip.framerate;
    }
}

void SectionScheduler::clearAllContinuation() {
    auto view = m_registry.view<ClipPlaybackPhase>();
    for (auto entity : view) {
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);
        phase.inContinuation = false;
        phase.sourcePhaseFrames = 0.0;
    }
}

} // namespace entity
