#include "entity/director/CatalogClipMath.hpp"

#include "entity/timeline/PlaybackWrap.hpp"

#include <algorithm>
#include <cmath>

namespace entity {

Clip clipFromCatalog(const bus::ClipCatalogEntry& e) {
    Clip c;
    c.startFrame       = e.startFrame;
    c.duration         = e.duration;
    c.mediaStartFrame  = e.mediaStartFrame;
    c.mediaOutFrame    = e.mediaOutFrame;
    c.totalMediaFrames = e.totalMediaFrames;
    c.framerate        = e.framerate;
    c.playbackMode     = static_cast<PlaybackMode>(e.playbackMode);
    c.sectionBehavior  = static_cast<SectionBehavior>(e.sectionBehavior);
    // targetScreen: convert from uint64 back to entt::entity
    c.targetScreen = (e.targetScreen == UINT64_MAX)
        ? entt::null
        : static_cast<entt::entity>(static_cast<std::uint32_t>(e.targetScreen));
    return c;
}

CatalogMediaFrameResult mapToMediaFrameFromCatalogEx(
        const bus::ClipCatalogEntry& e,
        FrameNumber timelineFrame,
        double timelineFrameRate,
        std::int64_t nowNs) {
    CatalogMediaFrameResult result;

    const Clip clip = clipFromCatalog(e);
    const FrameNumber sourceLength = effectivePlaybackLength(clip);
    // Normal-mode break-aligned extension (2026-05-23): realEnd extends past
    // clipEnd for Normal clips ending exactly at a break with source content
    // past that point; for Locked / non-break-aligned clips realEnd ==
    // clipEnd. All "is this clip past its play window?" checks use realEnd
    // so natural playback continues through the extension window before
    // tail-hold semantics kick in.
    const FrameNumber realEnd = clip.startFrame +
        entity::computeExtendedDuration(clip, timelineFrameRate,
                                        e.endAlignsWithSectionBreak,
                                        e.endingBreakFadeSeconds);

    // Tail short-circuit (same as the entity-aware overload) — applies past
    // the clip's real end, which is extendedEnd for Normal-extended clips.
    if (timelineFrame >= realEnd && e.hasPhase && e.phase_tailHoldMediaFrame >= 0) {
        result.mediaFrame = e.phase_tailHoldMediaFrame;
        result.inTailHold = true;
        return result;
    }

    const bool inContinuation = e.hasPhase && e.phase_inContinuation
        && clip.sectionBehavior == SectionBehavior::Normal;

    if (!inContinuation) {
        // post-break anchor path — covers the post-GO span, including the
        // extension window for Normal-extended clips.
        //
        // Defensive guard (2026-05-23) — only apply the anchor when the
        // playhead is at or past the anchor's reference timeline frame.
        // `resetAnchorsAcrossScrub` is supposed to clear stale anchors on
        // scrub-back-past-break and on Stop, but its delta-based
        // discontinuity detection can miss slow drags or clip-move
        // workflows where the playhead ends up before the anchor without
        // a single big jump. With a stale anchor the math's `max(...,0)`
        // clamp pins mediaFrame at the held value for every tick where
        // currentTimelineFrame < anchorTimelineFrame, which presents as
        // a video freeze. The natural sourceLocalFrame branch below
        // produces the correct mapping for the clip's current position.
        if (e.hasPhase && e.phase_postBreakMediaAnchor >= 0 &&
            timelineFrame >= e.phase_anchorTimelineFrame &&
            timelineFrame < realEnd) {
            if (sourceLength <= 0) {
                result.mediaFrame = clip.mediaStartFrame;
                return result;
            }
            const double frameRateRatio = (timelineFrameRate > 0.0)
                ? clip.framerate / timelineFrameRate : 1.0;
            const double timelineDelta =
                static_cast<double>(timelineFrame - e.phase_anchorTimelineFrame);
            const double localFloat =
                static_cast<double>(e.phase_postBreakMediaAnchor - clip.mediaStartFrame)
                + timelineDelta * frameRateRatio;
            const FrameNumber sourceLocalFrame = static_cast<FrameNumber>(
                std::floor(std::max(localFloat, 0.0)));

            const WrapResult w =
                wrapLocalFrame(clip.playbackMode, sourceLength, sourceLocalFrame);
            result.mediaFrame      = clip.mediaStartFrame + w.frame;
            result.pingPongReverse = w.reverse;
            return result;
        }

        // natural timeline-derived path (2-arg equivalent, inlined)
        const double frameRateRatio = (timelineFrameRate > 0.0)
            ? clip.framerate / timelineFrameRate : 1.0;
        // Fix 5 (2026-05-23) — held last decoded frame is the media frame
        // the clip displayed at its last authored timeline frame
        // (realEnd - 1), wrapped per playbackMode — NOT the trimmed
        // source-range end. A clip whose authored duration is shorter
        // than its trimmed source range never decoded mediaStartFrame +
        // sourceLength - 1, so asking the cache for that frame is a
        // guaranteed stall until SeekSyncController times out. For
        // Normal-extended clips realEnd > clipEnd so the natural-wrap
        // math walks through the extension window before clamping — the
        // PlaybackMode (Freeze/Loop/PingPong) wrap at sourceLocalFrame
        // >= sourceLength kicks in naturally at the source out point.
        if (timelineFrame >= realEnd) {
            timelineFrame = realEnd - 1;
            result.inTailHold = true;
        }
        const FrameNumber localFrame = timelineFrame - clip.startFrame;
        const FrameNumber sourceLocalFrame = static_cast<FrameNumber>(
            std::floor(localFrame * frameRateRatio));
        const WrapResult w =
            wrapLocalFrame(clip.playbackMode, sourceLength, sourceLocalFrame);
        result.mediaFrame      = clip.mediaStartFrame + w.frame;
        result.pingPongReverse = w.reverse;
        return result;
    }

    // continuation path: derive from accumulated source phase.
    if (sourceLength <= 0) {
        result.mediaFrame = clip.mediaStartFrame;
        return result;
    }
    // NEW-08: when a wall-clock anchor is present, re-derive the live phase
    // from it instead of the snapshot-frozen phase_sourcePhaseFrames. During
    // an editor stall no new SceneSnapshot is published, so the baked
    // phase_sourcePhaseFrames goes stale — but the anchor (set once at the
    // at-break park, never mutated during continuation) plus the caller-
    // supplied nowNs (from the active RateSource) keeps Loop/PingPong clips
    // cycling on the projector on the same clock domain as the seed.
    // Mirrors SectionScheduler::advanceContinuation's wall-clock path;
    // the dt-accumulator fallback (anchor == 0) reads the baked value.
    double effectivePhase = e.phase_sourcePhaseFrames;
    if (e.phase_continuationStartTimeNs > 0) {
        const double elapsedSec =
            static_cast<double>(nowNs - e.phase_continuationStartTimeNs) * 1e-9;
        effectivePhase = e.phase_continuationSeedFrames + elapsedSec * clip.framerate;
    }
    const double phaseClamped = std::max(effectivePhase, 0.0);
    const FrameNumber phaseFrame = static_cast<FrameNumber>(std::floor(phaseClamped));
    // Freeze clamps to the last source frame instead of wrapping. (The
    // pre-clamp is redundant with wrapLocalFrame's Freeze case — min then
    // passthrough equals the Freeze wrap for phaseFrame >= 0 — but keeping
    // it makes this path's behavior locally obvious.)
    const FrameNumber continuationLocal =
        (clip.playbackMode == PlaybackMode::Freeze)
            ? std::min(phaseFrame, sourceLength - 1)
            : phaseFrame;
    const WrapResult w =
        wrapLocalFrame(clip.playbackMode, sourceLength, continuationLocal);
    result.mediaFrame      = clip.mediaStartFrame + w.frame;
    result.pingPongReverse = w.reverse;
    return result;
}

FrameNumber mapToMediaFrameFromCatalog(const bus::ClipCatalogEntry& e,
                                       FrameNumber timelineFrame,
                                       double timelineFrameRate,
                                       std::int64_t nowNs) {
    return mapToMediaFrameFromCatalogEx(e, timelineFrame, timelineFrameRate,
                                        nowNs).mediaFrame;
}

FrameNumber catalogSectionTailFrames(const bus::ClipCatalogEntry& e,
                                     double timelineFrameRate) {
    if (!e.endAlignsWithSectionBreak) return 0;
    const FrameNumber ramp = e.endingBreakFadeSeconds > 0.0
        ? static_cast<FrameNumber>(
              std::ceil(e.endingBreakFadeSeconds * timelineFrameRate))
        : 0;
    return std::max<FrameNumber>(1, ramp);
}

} // namespace entity
