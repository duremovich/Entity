#pragma once

#include "entity/components/Clip.hpp"   // PlaybackMode
#include "entity/core/Types.hpp"

namespace entity {

/**
 * One wrap of a local frame into [0, sourceLength) per PlaybackMode.
 *
 * Extracted from the four lockstep copies (ADR-0029 Decision 3):
 * CatalogClipMath's wrapSourceLocalFrame, PlaybackTimeAuthority's
 * mapToMediaFrame (2-arg and 3-arg), DecodeSystem::update's steering wrap,
 * and AudioSystem's steering wrap. Any new Freeze/Loop/PingPong wrap must
 * call this primitive instead of open-coding the switch.
 *
 * `frame` is SOURCE-LOCAL — video callers add clip.mediaStartFrame.
 * `reverse` is PingPong odd-cycle parity (always false for Freeze/Loop and
 * for the un-wrapped passthrough).
 *
 * Contract: sourceLength >= 1 (effectivePlaybackLength guarantees this at
 * every migrated call site). sourceLength <= 0 returns {0, false}
 * defensively so no caller can ever hit a modulo-by-zero. localFrame <
 * sourceLength (including negative) passes through unwrapped, matching the
 * historical copies.
 */
struct WrapResult {
    FrameNumber frame{0};
    bool reverse{false};
};

inline WrapResult wrapLocalFrame(PlaybackMode mode,
                                 FrameNumber sourceLength,
                                 FrameNumber localFrame) {
    if (sourceLength <= 0) return {0, false};
    if (localFrame < sourceLength) return {localFrame, false};
    switch (mode) {
        case PlaybackMode::Freeze:
            return {sourceLength - 1, false};
        case PlaybackMode::Loop:
            return {localFrame % sourceLength, false};
        case PlaybackMode::PingPong: {
            const FrameNumber cycle = localFrame / sourceLength;
            const FrameNumber pos   = localFrame % sourceLength;
            const bool reverse = (cycle % 2 == 1);
            return {reverse ? (sourceLength - 1 - pos) : pos, reverse};
        }
    }
    return {sourceLength - 1, false};  // unreachable for a valid enum
}

} // namespace entity
