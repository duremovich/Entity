#include "entity/core/SeekSyncController.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/profile/Tracy.hpp"
#include <iostream>

namespace entity {

SeekSyncController::SeekSyncController(ReadinessFn videoReady, ReadinessFn audioReady)
    : m_videoReady(std::move(videoReady))
    , m_audioReady(std::move(audioReady))
{
}

void SeekSyncController::tick(Timeline* timeline)
{
    ZoneScopedN("SeekSyncController::tick");

    const bool gateNow = (timeline && timeline->isSeekSyncGated());

    if (!gateNow) {
        // Gate is clear — nothing to do.  Reset tracking state so the next
        // engage gets a clean slate.
        m_primed        = false;
        m_lastGateState = false;
        return;
    }

    if (!m_lastGateState) {
        // Gate just transitioned from clear → active.  Arm the timeout
        // measurement: read the engage timestamp that Timeline::play() stamped
        // at the moment play() was called.  This way the timeout is measured
        // from play()-time, not from the moment tick() first fires after play(),
        // which matters when (e.g.) a test sleeps between the two.
        m_primed        = true;
        m_lastGateState = true;
    }

    // -- Timeout failsafe --------------------------------------------------
    const int64_t engageNs  = timeline->getSeekSyncEngageTimeNs();
    const int64_t nowNs     = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
    const int64_t elapsedMs = (nowNs - engageNs) / 1'000'000LL;

    if (elapsedMs >= kPrerollTimeoutMs) {
        std::cerr << "[SeekSyncController] Preroll timeout after " << elapsedMs
                  << " ms — releasing gate. A/V sync may be degraded." << std::endl;
        timeline->setSeekSyncGate(false);
        m_primed        = false;
        m_lastGateState = false;
        return;
    }

    // -- Readiness poll ----------------------------------------------------
    const bool videoOk = (!m_videoReady || m_videoReady());
    const bool audioOk = (!m_audioReady || m_audioReady());

    if (videoOk && audioOk) {
        timeline->setSeekSyncGate(false);
        m_primed        = false;
        m_lastGateState = false;
        // No log on the normal fast path — it fires every play at frame 0
        // and would drown the console.
    }
}

} // namespace entity
