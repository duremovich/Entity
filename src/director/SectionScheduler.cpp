#include "entity/director/SectionScheduler.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/profile/Tracy.hpp"

#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace entity {

namespace {
    // A clip is "at the break" iff its start frame is exactly the break
    // frame. Frame-native section storage makes this an exact integer test;
    // the pre-migration ±1 tolerance only existed to absorb us->frame
    // truncation fuzz, which no longer exists.
    inline bool isAtBreakAlignedStart(FrameNumber clipStart, FrameNumber breakTimelineFrame) {
        return clipStart == breakTimelineFrame;
    }

}

SectionScheduler::SectionScheduler(entt::registry& registry, Timeline* timeline)
    : m_registry(registry)
    , m_timeline(timeline)
{}

void SectionScheduler::tick(double deltaTimeSeconds) {
    ZoneScopedN("SectionScheduler::tick");
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
        // Round-2 fixup, Phase 4 — Stopped is effectively a scrub away
        // from playback. Anchors set at frames past the current playhead
        // are stale and would otherwise re-engage on the next play.
        resetAnchorsAcrossScrub(currentTime);
        return;
    }

    if (!m_haveLastTickFrame) {
        m_lastTickFrame = currentTime;
        m_haveLastTickFrame = true;
        return;
    }

    if (state != PlaybackState::Playing) {
        // Paused (manually or by the at-break park). While at-break, advance
        // Normal-clip continuation phase so Loop / PingPong clips keep
        // cycling. The show-side mapToMediaFrameFromCatalog derives the same
        // value from the wall-clock anchor (so the projector stays correct
        // during editor stalls); this editor-side write keeps the registry
        // ClipPlaybackPhase component live for the PropertyWindow and is the
        // value go() snapshots. If the user just scrubbed elsewhere we'll
        // detect a discontinuity below.
        if (m_atBreak) {
            advanceContinuation(deltaTimeSeconds);
        }
        m_lastTickFrame = currentTime;
        return;
    }

    // Manual-resume unstick: any state-Playing tick where m_atBreak is
    // still raised is stale. Drop the latch unconditionally — Timeline::seek
    // already clears `sectionAtBreak()`, and the discontinuity branch
    // (below) clears scheduler state. This guard is the last safety net for
    // paths that don't go through either. The previous
    // `currentTime > m_lastBreakHitFrame` guard failed when the user
    // scrubbed BACKWARD before pressing Play.
    if (m_atBreak) {
        m_atBreak = false;
        m_timeline->setSectionAtBreak(false);
        clearAllContinuation();
    }

    // NEW-08: section-break *crossing detection* lives on the show thread
    // now (Engine::showThreadMain) — it runs every show frame regardless of
    // editor-thread health, so a break can no longer be missed by an editor
    // stall. The editor applies the crossing via handleBreakAt() from the
    // R2D drain. What remains here is scrub handling: if the playhead jumped
    // during playback the user scrubbed, and any post-break anchor past the
    // new playhead position is now stale.
    const double frameRate = m_timeline->getFrameRate() > 0.0 ? m_timeline->getFrameRate() : 30.0;
    const Timecode oneFrame = static_cast<Timecode>(1000000.0 / frameRate);

    // Scrub jumps vs continuous playback. The threshold is "more than 2
    // timeline frames OR more than 5× the per-tick advance, whichever is
    // greater". Without the deltaTime floor, high-fps timelines (>120 fps)
    // put 2× oneFrame below the per-render-tick advance (~16ms at 60Hz) and
    // every normal advance reads as a scrub.
    const Timecode delta = currentTime > m_lastTickFrame
                         ? currentTime - m_lastTickFrame
                         : m_lastTickFrame - currentTime;
    const Timecode tickAdvance = deltaTimeSeconds > 0.0
        ? static_cast<Timecode>(deltaTimeSeconds * 1000000.0)
        : static_cast<Timecode>(0);
    const Timecode discontinuityThreshold = std::max(oneFrame * 2, tickAdvance * 5);
    if (delta > discontinuityThreshold) {
        // A discontinuity here means the user scrubbed (or some non-playback
        // driver moved the playhead). Any post-break anchor whose
        // `anchorTimelineFrame > currentTimelineFrame` is now stale: the
        // user jumped backward past the break that set it, or forward past
        // the at-break pause without experiencing it.
        resetAnchorsAcrossScrub(currentTime);
        // Continuation is cleared too: the parked phase no longer
        // corresponds to the new playhead position.
        clearAllContinuation();
    }

    m_lastTickFrame = currentTime;
}

void SectionScheduler::handleBreakAt(FrameNumber breakFrame) {
    ZoneScopedN("SectionScheduler::handleBreakAt");
    // NEW-08 — apply a crossing the show thread detected. The show thread
    // has already snapped the playhead to the break, paused, and raised
    // Timeline::sectionAtBreak(); this runs the registry-mutating tail on
    // the editor thread (the sole registry writer, ADR-0014).
    if (!m_timeline) return;

    // Staleness guard. Between the show thread posting SectionBreakDetected
    // and this drain, a seek (clears sectionAtBreak) or a Play (flips state
    // back to Playing) may have landed — Timeline::seek calls
    // setSectionAtBreak(false). If either happened the message is stale; the
    // playhead is no longer parked at the break, so do not seed continuation.
    if (!m_timeline->sectionAtBreak()) return;
    if (m_timeline->getPlaybackState() != PlaybackState::Paused) return;

    // Idempotency: already parked at this exact break (e.g. the editor's own
    // path raced the drain). Re-seeding would reset the wall-clock anchors.
    if (m_atBreak && m_lastBreakHitFrame == breakFrame) return;

    m_atBreak           = true;
    m_lastBreakHitFrame = breakFrame;
    // m_lastTickFrame / m_haveLastTickFrame must move with the playhead —
    // otherwise the next tick() reads a bogus discontinuity off a stale
    // previous-frame value. m_lastTickFrame is the microsecond playhead, so
    // convert the integer break frame through frameToTime.
    m_lastTickFrame     = m_timeline->frameToTime(breakFrame);
    m_haveLastTickFrame = true;
    // Seed Normal clips with the source-frame phase they had at the break so
    // their continuation picks up smoothly.
    seedContinuationAt(breakFrame);

    std::cout << "[SectionScheduler] At break frame=" << breakFrame
              << " (applied from show-thread detection)" << std::endl;
}

void SectionScheduler::go() {
    ZoneScopedN("SectionScheduler::go");
    if (!m_timeline || !m_atBreak) return;

    // breakFrame and resumeFrame are integer timeline frames — resume one
    // frame past the break.
    const FrameNumber resumeFrame = m_lastBreakHitFrame + 1;

    // NEW-08 — recompute continuation phase from the wall-clock anchor
    // BEFORE the snapshot helpers below. advanceContinuation's wall-clock
    // path derives sourcePhaseFrames from (now - continuationStartTimeNs),
    // independent of dt, so a 0.0 dt is fine. Without this, an editor that
    // stalled through the at-break park leaves phase.sourcePhaseFrames at
    // the seed value (advanceContinuation didn't tick during the stall),
    // and snapshotTailHoldFrames / snapshotPostBreakAnchors would capture a
    // source frame the user never saw — a visible jump at GO.
    advanceContinuation(0.0);

    // Phase 6 — snapshot the displayed frame for any continuing clip
    // ending at this break, so the post-break held-fade shows what the
    // user actually saw. Must run BEFORE clearAllContinuation, otherwise
    // the math falls back to the (frozen) timeline-derived path and a
    // looping clip would snapshot the wrong frame.
    snapshotTailHoldFrames(m_lastBreakHitFrame);

    // Round-2 fixup, Phase 4 — for clips that span the break (don't end
    // at it), capture a post-break source-frame anchor so the 3-arg
    // mapToMediaFrame can resume from where the user was watching at GO
    // instead of rewinding to the natural clipStart-derived mapping. Same
    // ordering rationale as snapshotTailHoldFrames: must run BEFORE
    // clearAllContinuation while `inContinuation == true` and
    // `sourcePhaseFrames` still reflect the at-break advance.
    snapshotPostBreakAnchors(m_lastBreakHitFrame);

    // Clear continuation BEFORE flipping the at-break flag so the very next
    // mapToMediaFrame call (post-resume) takes the anchor-derived path
    // (or the timeline-derived path for non-spanning clips).
    clearAllContinuation();

    m_atBreak = false;
    m_timeline->setSectionAtBreak(false);
    m_timeline->seekToFrame(resumeFrame);
    m_timeline->play();
    m_lastTickFrame = m_timeline->frameToTime(resumeFrame);
    m_haveLastTickFrame = true;

    std::cout << "[SectionScheduler] GO from break " << m_lastBreakHitFrame
              << " -> resume at " << resumeFrame << std::endl;
}

void SectionScheduler::seedContinuationAt(FrameNumber breakTimelineFrame) {
    ZoneScopedN("SectionScheduler::seedContinuationAt");
    if (!m_timeline) return;
    const double timelineFrameRate = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;

    auto view = m_registry.view<Clip>();
    for (auto entity : view) {
        const Clip& clip = view.get<Clip>(entity);

        // Only Normal-mode clips that are active at the break get continuation.
        if (clip.sectionBehavior != SectionBehavior::Normal) continue;
        // Symmetric with advanceContinuation's guard. A clip with framerate <= 0
        // (uninitialized / corrupt project) would compute a zero phase and
        // silently land at mediaStartFrame regardless of where the break sat.
        if (clip.framerate <= 0.0) continue;
        // 2026-05-23 — Normal-mode break-aligned extension. Use the extended
        // duration (computed by PlaybackTimeAuthority from source out-point)
        // so a clip whose end aligns with this break AND has source content
        // past the break still gets continuation — its source-phase advances
        // via wall-clock during the at-break park, then postBreakMediaAnchor
        // resumes natural playback past clipEnd to extendedEnd after GO.
        // For Locked / non-break-aligned clips, extendedDuration == duration
        // so the original spanning check is preserved exactly.
        // See ADR-0012 amendment 2026-05-23.
        const FrameNumber clipActiveEnd = m_timeAuthority
            ? clip.startFrame + m_timeAuthority->computeExtendedDuration(clip)
            : clip.startFrame + clip.duration;
        if (breakTimelineFrame < clip.startFrame ||
            breakTimelineFrame >= clipActiveEnd) {
            continue;
        }

        // Lazy-allocate phase for both queued and spanning paths so
        // snapshotPostBreakAnchors can stamp on the same component.
        ClipPlaybackPhase& phase = m_registry.get_or_emplace<ClipPlaybackPhase>(entity);

        if (isAtBreakAlignedStart(clip.startFrame, breakTimelineFrame)) {
            // Queued clip — does NOT accumulate continuation. The post-GO
            // anchor is stamped in snapshotPostBreakAnchors so playback
            // flows through the existing anchor branch, just like a
            // spanning clip would after its own break.
            phase.inContinuation = false;
            phase.sourcePhaseFrames = 0.0;
            phase.continuationStartTimeNs = 0;
            phase.continuationSeedFrames = 0.0;
            continue;
        }

        // Source-frame phase at the break = (timeline delta since clip
        // start) * (clipFps / timelineFps).
        const double deltaTimelineFrames =
            static_cast<double>(breakTimelineFrame - clip.startFrame);
        const double frameRateRatio = clip.framerate / timelineFrameRate;
        // Round-2 fixup, Phase 4 — multi-break: when a clip spans break-A
        // (which set an anchor) and reaches break-B, seed the new
        // continuation phase from the carry-forward anchor instead of the
        // natural (clipStart-derived) mapping. Otherwise the at-break-B
        // pause would visibly jump backward to whatever clip-start delta
        // happens to land on.
        if (phase.postBreakMediaAnchor >= 0) {
            const double timelineDelta =
                static_cast<double>(breakTimelineFrame - phase.anchorTimelineFrame);
            phase.sourcePhaseFrames =
                static_cast<double>(phase.postBreakMediaAnchor - clip.mediaStartFrame)
                + timelineDelta * frameRateRatio;
        } else {
            phase.sourcePhaseFrames = deltaTimelineFrames * frameRateRatio;
        }
        phase.inContinuation = true;
        // Round-3 fixup: anchor the continuation phase to wall-clock at
        // entry. advanceContinuation recomputes sourcePhaseFrames from
        // (now - continuationStartTimeNs) * fps + continuationSeedFrames
        // each tick, so phase tracks wall time exactly regardless of
        // editor tick-rate fluctuations or dt accumulation error.
        phase.continuationStartTimeNs = m_timeAuthority
            ? static_cast<int64_t>(m_timeAuthority->rateNow() * 1e9)
            : int64_t{0};
        phase.continuationSeedFrames = phase.sourcePhaseFrames;
    }

    // Freeze Locked OA layers that are active at the break.
    // Normal OA layers are left running (no frozen flag set); they continue
    // to be re-evaluated by AnimationSystem every tick.
    auto oaView = m_registry.view<ObjectAnimationLayer, Layer>();
    for (auto entity : oaView) {
        auto& oal = oaView.get<ObjectAnimationLayer>(entity);
        if (oal.sectionBehavior != SectionBehavior::Locked) continue;
        const auto& lay = oaView.get<Layer>(entity);
        if (breakTimelineFrame < lay.startFrame ||
            breakTimelineFrame >= lay.startFrame + lay.duration) {
            continue;
        }
        oal.frozen = true;
    }
}

void SectionScheduler::advanceContinuation(double deltaTimeSeconds) {
    ZoneScopedN("SectionScheduler::advanceContinuation");
    // Round-3 fixup: prefer the wall-clock anchor (continuationStartTimeNs)
    // over the dt accumulator. The accumulator is mathematically equivalent
    // when dt is correctly per-editor-tick wall-clock, but it amplifies any
    // dt mis-measurement (e.g., editor reading a stale show-thread cached
    // dt while ticking faster) and drifts under fp accumulation.
    // Wall-clock anchor: phase = seed + (now - start) * fps. Exact, always.
    //
    // The dt fallback (continuationStartTimeNs == 0) preserves the
    // synthetic-time path used by tests that drive advanceContinuation
    // without going through seedContinuationAt's anchor capture.
    const int64_t nowNs = m_timeAuthority
        ? static_cast<int64_t>(m_timeAuthority->rateNow() * 1e9)
        : int64_t{0};
    auto view = m_registry.view<Clip, ClipPlaybackPhase>();
    for (auto entity : view) {
        const Clip& clip = view.get<Clip>(entity);
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);
        if (!phase.inContinuation) continue;
        if (clip.sectionBehavior != SectionBehavior::Normal) continue;
        if (clip.framerate <= 0.0) continue;

        if (phase.continuationStartTimeNs > 0) {
            const double elapsedSec =
                static_cast<double>(nowNs - phase.continuationStartTimeNs) * 1e-9;
            phase.sourcePhaseFrames =
                phase.continuationSeedFrames + elapsedSec * clip.framerate;
        } else if (deltaTimeSeconds > 0.0) {
            phase.sourcePhaseFrames += deltaTimeSeconds * clip.framerate;
        }
    }
}

void SectionScheduler::clearAllContinuation() {
    auto view = m_registry.view<ClipPlaybackPhase>();
    for (auto entity : view) {
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);
        phase.inContinuation = false;
        phase.sourcePhaseFrames = 0.0;
        // Round-3 fixup: drop the wall-clock anchor with the rest of the
        // continuation state. Next at-break park reseeds via seedContinuationAt.
        phase.continuationStartTimeNs = 0;
        phase.continuationSeedFrames = 0.0;
        // tailHoldMediaFrame is intentionally left intact when called from
        // go() — it's seeded just above by snapshotTailHoldFrames and read
        // by mapToMediaFrame during the post-end fade tail. The Stopped /
        // unstick paths reach this through the same routine; they don't
        // produce a fade tail (no resume into a break-aligned clip end),
        // so a stale value sitting on a re-seeded clip is harmless until
        // the next snapshotTailHoldFrames overwrites or seedContinuationAt
        // (which doesn't touch the field) leaves it alone.
        //
        // Round-2 fixup, Phase 4 — postBreakMediaAnchor / anchorTimelineFrame
        // are likewise preserved here. They were just seeded above by
        // snapshotPostBreakAnchors and the post-GO 3-arg mapToMediaFrame
        // path needs them to keep playback rolling without a rewind.
        // Anchor invalidation happens via resetAnchorsAcrossScrub on
        // discontinuities and on Stopped, not here.
    }

    // Unfreeze all OA layers so AnimationSystem resumes normal evaluation
    // after GO (or Stop, where the user scrubs to a new position).
    auto oaView = m_registry.view<ObjectAnimationLayer>();
    for (auto entity : oaView) {
        oaView.get<ObjectAnimationLayer>(entity).frozen = false;
    }
}

void SectionScheduler::snapshotTailHoldFrames(FrameNumber breakTimelineFrame) {
    if (!m_timeline) return;

    auto view = m_registry.view<Clip, ClipPlaybackPhase>();
    for (auto entity : view) {
        const Clip& clip = view.get<Clip>(entity);
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);

        if (!phase.inContinuation) continue;
        if (clip.sectionBehavior != SectionBehavior::Normal) continue;
        if (clip.framerate <= 0.0) continue;

        // Only clips whose end is exactly on this break get a tail
        // snapshot; others won't enter the post-end fade window at all.
        const FrameNumber clipEnd = clip.startFrame + clip.duration;
        if (clipEnd != breakTimelineFrame) continue;

        // 2026-05-23 — Normal-mode break-aligned extension. A clip ending
        // at this break that has more source content to play (extended
        // duration > duration) is NOT entering a tail; it's playing
        // through to extendedEnd via postBreakMediaAnchor after GO. Skip
        // the tail-hold snapshot — the held-frame branch in mapToMediaFrame
        // is gated on `timelineFrame >= realEnd`, and for extended clips
        // realEnd == extendedEnd, well past breakFrame. Leaving the
        // tail-hold unset (sentinel -1) means the held-frame branch
        // simply doesn't fire inside the extension window.
        // See ADR-0012 amendment 2026-05-23.
        if (m_timeAuthority &&
            m_timeAuthority->computeExtendedDuration(clip) > clip.duration) {
            continue;
        }

        const FrameNumber sourceLength = effectivePlaybackLength(clip);
        if (sourceLength <= 0) continue;

        const double phaseClamped = std::max(phase.sourcePhaseFrames, 0.0);
        const FrameNumber phaseFrame = static_cast<FrameNumber>(std::floor(phaseClamped));

        FrameNumber held = clip.mediaStartFrame + sourceLength - 1;
        switch (clip.playbackMode) {
            case PlaybackMode::Freeze:
                held = clip.mediaStartFrame + std::min(phaseFrame, sourceLength - 1);
                break;
            case PlaybackMode::Loop:
                held = clip.mediaStartFrame + (phaseFrame % sourceLength);
                break;
            case PlaybackMode::PingPong: {
                const FrameNumber cycle = phaseFrame / sourceLength;
                const FrameNumber pos   = phaseFrame % sourceLength;
                held = (cycle % 2 == 0)
                    ? clip.mediaStartFrame + pos
                    : clip.mediaStartFrame + (sourceLength - 1 - pos);
                break;
            }
        }
        phase.tailHoldMediaFrame = held;
    }
}

void SectionScheduler::snapshotPostBreakAnchors(FrameNumber breakTimelineFrame) {
    if (!m_timeline) return;

    auto view = m_registry.view<Clip, ClipPlaybackPhase>();
    for (auto entity : view) {
        const Clip& clip = view.get<Clip>(entity);
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);

        if (clip.sectionBehavior != SectionBehavior::Normal) continue;
        if (clip.framerate <= 0.0) continue;

        // Skip clips ending exactly AT the break — those are owned by
        // tail-hold, they don't play past the break, so they don't need
        // an anchor.
        //
        // 2026-05-23 — exception: Normal-mode break-aligned extension.
        // A clip ending at this break that has source content past it
        // (extendedDuration > duration) IS going to play through the
        // break, up to extendedEnd. It needs the anchor so the 3-arg
        // mapToMediaFrame's post-break path resumes from the source
        // frame the user was watching at GO instead of rewinding to
        // (clip.startFrame, mediaStartFrame). See ADR-0012 amendment
        // 2026-05-23.
        const FrameNumber clipEnd = clip.startFrame + clip.duration;
        if (clipEnd == breakTimelineFrame) {
            const bool hasExtension = m_timeAuthority &&
                m_timeAuthority->computeExtendedDuration(clip) > clip.duration;
            if (!hasExtension) continue;
        }

        if (isAtBreakAlignedStart(clip.startFrame, breakTimelineFrame)) {
            // Queued clip — first visible post-GO tick is clipStart+1,
            // mapping to mediaStartFrame. Stamp the anchor with that
            // pairing; the existing anchor branch in mapToMediaFrame
            // (and the decoder's anchor steer) handles every subsequent
            // tick naturally. Must run BEFORE the !inContinuation guard
            // because seedContinuationAt leaves queued phase with
            // inContinuation=false.
            phase.postBreakMediaAnchor = clip.mediaStartFrame;
            phase.anchorTimelineFrame = clip.startFrame + 1;
            continue;
        }

        if (!phase.inContinuation) continue;

        // Compute the source-media frame the clip is currently mapping
        // to — same Freeze/Loop/PingPong wrap math the 3-arg
        // mapToMediaFrame uses on `sourcePhaseFrames`. We can't call
        // PlaybackTimeAuthority from here (Director-side ownership cut),
        // so mirror the math inline.
        const FrameNumber sourceLength = effectivePlaybackLength(clip);
        if (sourceLength <= 0) continue;

        const double phaseClamped = std::max(phase.sourcePhaseFrames, 0.0);
        const FrameNumber phaseFrame = static_cast<FrameNumber>(std::floor(phaseClamped));

        FrameNumber currentSrc = clip.mediaStartFrame + sourceLength - 1;
        switch (clip.playbackMode) {
            case PlaybackMode::Freeze:
                currentSrc = clip.mediaStartFrame + std::min(phaseFrame, sourceLength - 1);
                break;
            case PlaybackMode::Loop:
                currentSrc = clip.mediaStartFrame + (phaseFrame % sourceLength);
                break;
            case PlaybackMode::PingPong: {
                const FrameNumber cycle = phaseFrame / sourceLength;
                const FrameNumber pos   = phaseFrame % sourceLength;
                currentSrc = (cycle % 2 == 0)
                    ? clip.mediaStartFrame + pos
                    : clip.mediaStartFrame + (sourceLength - 1 - pos);
                break;
            }
        }

        phase.postBreakMediaAnchor = currentSrc;
        // Anchor's reference frame is the post-GO timeline position (one
        // frame past the break). The 3-arg mapToMediaFrame computes
        // `(timelineFrame - anchorTimelineFrame) * ratio` as the delta
        // off the anchor — so at frame breakTimelineFrame+1 the delta is
        // zero and the returned media frame equals the anchor exactly.
        phase.anchorTimelineFrame = breakTimelineFrame + 1;
    }
}

void SectionScheduler::resetAnchorsAcrossScrub(Timecode currentTime) {
    if (!m_timeline) return;
    const FrameNumber currentTimelineFrame = m_timeline->timeToFrame(currentTime);

    auto view = m_registry.view<ClipPlaybackPhase>();
    for (auto entity : view) {
        ClipPlaybackPhase& phase = view.get<ClipPlaybackPhase>(entity);
        if (phase.postBreakMediaAnchor < 0) continue;
        // The anchor was set at break-N for play resuming at break-N+1.
        // If the user is now at a timeline frame BEFORE that resume
        // point, they either scrubbed backward past the break or
        // forward-jumped to skip the at-break pause; either way the
        // anchor should not be applied to the new playback position.
        if (phase.anchorTimelineFrame > currentTimelineFrame) {
            phase.postBreakMediaAnchor = -1;
            phase.anchorTimelineFrame = 0;
        }
    }
}

} // namespace entity
