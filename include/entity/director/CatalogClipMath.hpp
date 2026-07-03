#pragma once

// Catalog-side clip math shared by everything that maps a timeline frame to
// a media frame from a bus::ClipCatalogEntry instead of the live registry:
// PlaybackTimeAuthority::buildRenderFrame (show thread), and the
// DecodeSystem/AudioSystem stall-fallback ticks (issue #74). Pure functions —
// no registry, no Timeline, no locks — so they are callable from any thread.
//
// The math mirrors the registry-aware mapToMediaFrame path in
// PlaybackTimeAuthority (and the steering math in DecodeSystem::update);
// keep the two in lockstep when touching wrap/anchor/continuation behavior.

#include "entity/bus/Message.hpp"
#include "entity/components/Clip.hpp"

#include <cstdint>

namespace entity {

// Reconstruct a Clip value from a ClipCatalogEntry for use with the pure
// mapToMediaFrame(const Clip&, FrameNumber) overload, isClipActiveAtFrame,
// computeSectionFadeMultiplier, and computeExtendedDuration.
// FFmpeg pointer fields stay null — only the scheduling/math fields matter.
Clip clipFromCatalog(const bus::ClipCatalogEntry& e);

// Extended mapping result for callers that steer decode workers (the stall
// fallback): the frame plus the two flags DecodeSystem's steering needs.
struct CatalogMediaFrameResult {
    FrameNumber mediaFrame{0};
    // PingPong cycle parity of whichever branch produced mediaFrame (true =
    // odd cycle, playing in reverse). Always false for Freeze/Loop.
    bool pingPongReverse{false};
    // True when the clip is past its real end and mediaFrame is a held
    // frame (phase tail-hold or the realEnd-1 clamp): steer with no
    // decode-ahead and skip cache-miss recovery, mirroring the editor
    // path's inTailHeld handling.
    bool inTailHold{false};
};

// Show-side mapToMediaFrame: mirrors the entity-aware 3-arg overload but
// reads phase data from the ClipCatalogEntry instead of the registry.
// nowNs is the active rate source's current time in nanoseconds (callers
// pass rateNow()*1e9 so the continuation anchor and the show-thread
// re-derivation share the same clock domain as the editor-side seed).
CatalogMediaFrameResult mapToMediaFrameFromCatalogEx(
    const bus::ClipCatalogEntry& e,
    FrameNumber timelineFrame,
    double timelineFrameRate,
    std::int64_t nowNs);

// Frame-only convenience wrapper (buildRenderFrame's original interface).
FrameNumber mapToMediaFrameFromCatalog(const bus::ClipCatalogEntry& e,
                                       FrameNumber timelineFrame,
                                       double timelineFrameRate,
                                       std::int64_t nowNs);

// Section-fade tail length at the clip's end, derived from the BAKED
// catalog fields — the mirror of timeline::sectionFadeTailFrames' formula
// evaluated at snapshot time (>=1 hold frame when the end aligns with a
// break, plus ceil(fadeSeconds * fps) ramp frames). Lets the stall-fallback
// window gates consume the snapshot only (no Timeline section lock + vector
// copy per entry per tick) and guarantees the gate agrees with
// mapToMediaFrameFromCatalogEx's realEnd math, which uses the same baked
// fields. Keep in lockstep with timeline::sectionFadeTailFrames.
FrameNumber catalogSectionTailFrames(const bus::ClipCatalogEntry& e,
                                     double timelineFrameRate);

} // namespace entity
