#include "entity/director/SectionScheduler.hpp"

#include "entity/timeline/Timeline.hpp"

#include <cstdlib>
#include <iostream>

namespace entity {

SectionScheduler::SectionScheduler(Timeline* timeline)
    : m_timeline(timeline)
{}

void SectionScheduler::tick() {
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
        return;
    }

    if (!m_haveLastTickFrame) {
        m_lastTickFrame = currentTime;
        m_haveLastTickFrame = true;
        return;
    }

    if (state != PlaybackState::Playing) {
        // Paused (manually or by us). Track the playhead but don't fire crossings.
        // If the user just scrubbed elsewhere we'll detect a discontinuity below.
        m_lastTickFrame = currentTime;
        return;
    }

    const double frameRate = m_timeline->getFrameRate() > 0.0 ? m_timeline->getFrameRate() : 30.0;
    const Timecode oneFrame = static_cast<Timecode>(1000000.0 / frameRate);

    // Scrub jumps are not crossings — only continuous playback advances
    // trigger break detection. A delta of more than two frames since last
    // tick is treated as a discontinuity (user-driven seek).
    const Timecode delta = currentTime > m_lastTickFrame
                         ? currentTime - m_lastTickFrame
                         : m_lastTickFrame - currentTime;
    if (delta > oneFrame * 2) {
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

    m_atBreak = false;
    m_timeline->setSectionAtBreak(false);
    m_timeline->seek(resumeFrame);
    m_timeline->play();
    m_lastTickFrame = resumeFrame;
    m_haveLastTickFrame = true;

    std::cout << "[SectionScheduler] GO from break " << m_lastBreakHitFrame
              << " -> resume at " << resumeFrame << std::endl;
}

} // namespace entity
