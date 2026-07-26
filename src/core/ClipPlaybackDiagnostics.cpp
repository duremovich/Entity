#include "entity/core/ClipPlaybackDiagnostics.hpp"

#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/core/Engine.hpp"
#include "entity/director/Director.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/timeline/Timeline.hpp"

namespace entity {

ClipPlaybackDiagnostics gatherClipPlaybackDiagnostics(Engine& engine,
                                                      entt::entity clipEntity,
                                                      const Clip& clip) {
    ClipPlaybackDiagnostics d;

    auto* timeline = engine.getTimeline();
    if (!timeline) return d;

    d.timelineFrame = timeline->getCurrentFrame();
    d.localFrame    = d.timelineFrame - clip.startFrame;
    d.sourceLength  = effectivePlaybackLength(clip);

    auto* timeAuthority = engine.getDirector()
        ? engine.getDirector()->getTimeAuthority() : nullptr;

    // Active window uses the EXTENDED duration, matching the bound the frame
    // math uses. A Normal clip whose end aligns with a break plays past
    // clipEnd; it is on screen there, so a freeze there still counts.
    const FrameNumber activeEnd = clip.startFrame +
        (timeAuthority ? timeAuthority->computeExtendedDuration(clip)
                       : clip.duration);
    d.clipActive = d.timelineFrame >= clip.startFrame &&
                   d.timelineFrame <  activeEnd;

    if (timeAuthority) {
        d.mapped = timeAuthority->mapToMediaFrame(clipEntity, clip, d.timelineFrame);
    }

    if (auto* presenter = engine.getPlaybackPresenter()) {
        d.presented = presenter->presentedFrame(clipEntity);
    }

    if (auto* decodeSystem = engine.getDecodeSystem()) {
        if (auto worker = decodeSystem->getWorker(clipEntity)) {
            d.hasWorker     = true;
            d.decoderFrame  = worker->currentFrame.load();
            d.decoderTarget = worker->targetFrame.load();
            d.seeking       = worker->seekPending.load();
        }
    }

    if (auto* frameCache = engine.getFrameCache(); frameCache && d.mapped >= 0) {
        d.cacheHit = frameCache->has(clipEntity, d.mapped);
    }

    if (const auto* phase = engine.getRegistry().try_get<ClipPlaybackPhase>(clipEntity)) {
        d.inContinuation    = phase->inContinuation;
        d.sourcePhaseFrames = phase->sourcePhaseFrames;
    }

    return d;
}

} // namespace entity
