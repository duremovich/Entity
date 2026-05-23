#include "entity/timeline/SectionFade.hpp"

#include "entity/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>

namespace entity::timeline {

FrameNumber sectionFadeTailFrames(const Timeline& timeline,
                                  FrameNumber endFrame) {
    auto [sections, rawFps] = timeline.copySectionsAndRate();
    if (sections.empty()) return 0;
    const double timelineFrameRate = rawFps > 0.0 ? rawFps : 30.0;
    for (const auto& sec : sections) {
        // breakFrame is an integer timeline frame; the clip's end must be
        // exactly on the break to get a post-end hold + fade tail.
        if (sec.breakFrame != endFrame) continue;
        // Any clip whose end aligns with a break holds for at least 1
        // frame so it stays visible while the playhead is parked at the
        // break (independent of fadeSeconds). The hold frame is
        // breakFrame == clipEnd; post-GO behavior is governed by the
        // fade ramp (or instant cut when fadeSeconds == 0). Without
        // this, a trailing-edge clip with fadeSeconds=0 would pop out
        // the moment the playhead reaches the break.
        const FrameNumber ramp = sec.fadeSeconds > 0.0
            ? static_cast<FrameNumber>(std::ceil(sec.fadeSeconds * timelineFrameRate))
            : 0;
        return std::max<FrameNumber>(1, ramp);
    }
    return 0;
}

} // namespace entity::timeline
