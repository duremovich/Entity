#pragma once

#include "entity/core/Types.hpp"

#include <entt/entt.hpp>

namespace entity {

class Engine;
struct Clip;

/**
 * One snapshot of a clip's live transport state, gathered on the editor thread.
 *
 * The load-bearing pair is `mapped` vs `presented`. `mapped` is the source
 * frame the engine decided this clip should be showing; `presented` is the one
 * that actually reached its GPU texture. Every other number in the engine can
 * keep advancing while `presented` sits still — that state IS a frozen picture,
 * and it is invisible from `mapped` alone (which is why the section-break
 * freeze survived a passing AssertClipMediaFrame).
 *
 * Two consumers share this: the Clip Info "Playback (live)" panel and the
 * LogClipPlayback / AssertClipPresentedFrame script commands. They must agree —
 * the panel an operator reads and the assert a test trusts are supposed to be
 * the same instrument.
 */
struct ClipPlaybackDiagnostics {
    // Presented trailing mapped by a frame or two is the healthy steady state,
    // not a fault: the decoder runs slightly behind the playhead and the
    // presenter's nearest-frame fallback paints the closest frame it has. Only
    // a sustained gap means the picture has stopped tracking, so callers flag
    // on the size of the lag rather than on inequality — otherwise the readout
    // cries wolf on every normal tick.
    static constexpr FrameNumber kStaleLagFrames = 10;   // ~1/3 s at 30 fps

    FrameNumber timelineFrame{0};
    FrameNumber localFrame{0};
    FrameNumber sourceLength{0};

    FrameNumber mapped{INVALID_FRAME};
    FrameNumber presented{INVALID_FRAME};

    bool        hasWorker{false};
    FrameNumber decoderFrame{INVALID_FRAME};
    FrameNumber decoderTarget{INVALID_FRAME};
    bool        seeking{false};

    bool        cacheHit{false};

    bool        inContinuation{false};
    double      sourcePhaseFrames{0.0};

    // Playhead is inside the clip's *extended* window — the bound the frame
    // math actually uses (PlaybackTimeAuthority::computeExtendedDuration), not
    // startFrame + duration. A Normal clip whose end aligns with a section
    // break keeps playing past clipEnd, and it is still on screen there, so a
    // freeze in that window must still be reported.
    bool        clipActive{false};

    bool everPresented() const { return presented >= 0; }

    // Positive = presented is behind mapped.
    FrameNumber lag() const {
        return (mapped >= 0 && presented >= 0) ? (mapped - presented) : 0;
    }

    bool stale() const {
        if (!clipActive || mapped < 0 || presented < 0) return false;
        const FrameNumber l = lag();
        return l > kStaleLagFrames || l < -kStaleLagFrames;
    }
};

/**
 * Gather the live transport state for one clip. Editor thread only — reads the
 * registry, the DecodeSystem worker atomics, the FrameCache (internally
 * locked), and PlaybackPresenter's guarded presented-frame mirror.
 */
ClipPlaybackDiagnostics gatherClipPlaybackDiagnostics(Engine& engine,
                                                      entt::entity clipEntity,
                                                      const Clip& clip);

} // namespace entity
