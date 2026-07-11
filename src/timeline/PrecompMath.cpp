#include "entity/timeline/PrecompMath.hpp"

#include "entity/timeline/PlaybackWrap.hpp"

#include <algorithm>
#include <cmath>

namespace entity {

PrecompFrameResult mapOuterToInnerFrame(const PrecompInstanceParams& p,
                                        FrameNumber outerFrame,
                                        double outerTimelineFps) {
    PrecompFrameResult r;

    // ADR-0029 D3 edge cases: zero/negative instance duration or trim past
    // the definition end ⇒ inactive.
    if (p.instanceDuration <= 0) return r;
    if (outerFrame < p.instanceStartFrame ||
        outerFrame >= p.instanceStartFrame + p.instanceDuration) return r;

    const FrameNumber playLen = p.definitionDuration - p.innerStartFrame;
    if (playLen <= 0) return r;

    const double speed = std::clamp(p.speed, 0.01, 100.0);
    // Same fallback style as the clip math's frameRateRatio guard.
    const double fpsRatio = (outerTimelineFps > 0.0 && p.definitionFrameRate > 0.0)
        ? (p.definitionFrameRate / outerTimelineFps) : 1.0;

    const FrameNumber local = outerFrame - p.instanceStartFrame;
    const FrameNumber sourceLocal = static_cast<FrameNumber>(
        std::floor(static_cast<double>(local) * fpsRatio * speed));

    const WrapResult w = wrapLocalFrame(p.playbackMode, playLen, sourceLocal);
    r.active          = true;
    r.innerFrame      = p.innerStartFrame + w.frame;
    r.pingPongReverse = w.reverse;
    return r;
}

} // namespace entity
