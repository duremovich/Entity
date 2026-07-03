#pragma once
#include "entity/systems/System.hpp"
#include "entity/audio/AudioDecodeWorker.hpp"
#include "entity/profile/Tracy.hpp"
#include <entt/entt.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

namespace entity {

namespace bus { struct SceneSnapshot; }

class Timeline;
class AudioEngine;
class PlaybackTimeAuthority;

// Owns per-clip audio decode workers and drives them from the timeline.
// update() is editor-only (registry reads + worker lifecycle); the show
// thread's editor-stall path is tickFromSnapshot, which steers existing
// workers' atomics from the published SceneSnapshot and never touches the
// registry (ADR-0014 as amended by issue #74).
class AudioSystem : public System {
public:
    AudioSystem();
    ~AudioSystem();

    void setTimeline(Timeline* t)                    { m_timeline = t; }
    void setAudioEngine(AudioEngine* e)              { m_audioEngine = e; }
    void setTimeAuthority(PlaybackTimeAuthority* a)  { m_timeAuthority = a; }

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "AudioSystem"; }

    /**
     * Show-thread editor-stall fallback (issue #74, ADR-0014 amendment).
     * Drives existing workers' mixSource.active + seek atomics from the
     * published SceneSnapshot clip catalog — zero registry access, no
     * worker lifecycle, no gain/mute/solo mirroring (editor-only; the
     * atomics hold their last values through a stall). rateNowNs is the
     * active rate source's now in ns (PlaybackTimeAuthority::rateNow()*1e9).
     */
    void tickFromSnapshot(const bus::SceneSnapshot& scene,
                          std::int64_t rateNowNs);

    /**
     * Returns true when the audio worker for this entity has enough ring data
     * to start playback without an immediate underrun. Used by
     * SeekSyncController to decide when audio is primed after a seek.
     *
     * - No worker → false (wait for bootstrap)
     * - initFailed → true (don't block on broken media)
     * - else → initialized && !seekPending && ring.availableFrames() >= kAudioPrerollFrames
     */
    bool isWorkerSeekReady(entt::entity clipEntity) const;

    /**
     * Returns the clip-local media frame the audio worker was last steered to
     * (via seekTarget), converted from output-rate samples back to frames using
     * the clip's framerate.  Used by AssertAudioWorkerSeekFrame to verify
     * seek-sync positioning in integration tests.
     *
     * - No worker / not initialized: returns -1.
     * - initFailed: returns 0 (no meaningful position; worker never ran).
     * - else: (seekTarget * fps) / sampleRate, clamped to [0, INT64_MAX].
     */
    int64_t getWorkerSeekTargetFrame(entt::entity clipEntity, double clipFps) const;

    /**
     * Number of real seeks issued to this clip's audio worker since it was
     * created (AudioDecodeWorker::seekCount). -1 when no worker exists.
     * Test observability: AssertAudioWorkerSeekCountAtMost verifies the
     * stall fallback doesn't seek-storm on steering-authority handoffs
     * (issue #74 — each spurious seek is an audible ring-clear dropout).
     */
    int64_t getWorkerSeekCount(entt::entity clipEntity) const;

    // Minimum ring fill (~43 ms @ 48 kHz) required before SeekSyncController
    // considers an audio worker primed.
    static constexpr size_t kAudioPrerollFrames = 2048;

private:
    void createWorker(entt::entity e, entt::registry& registry);
    void destroyWorker(entt::entity e);

    // Retire-and-reap teardown (delete-freeze fix, mirrors DecodeSystem).
    // retireWorker unregisters the mix source and signals the thread to stop
    // WITHOUT joining, so a group delete doesn't serialize N thread-joins on
    // the editor tick. reapRetiredWorkers joins+erases once the thread has
    // set `finished`. Editor-thread only.
    void retireWorker(entt::entity e);
    void reapRetiredWorkers();

    // shared_ptr copy of the worker for `e` under m_workersMutex (nullptr if
    // absent). All readers — editor tick, show-thread stall fallback, the
    // seek-sync gate accessors — go through this; never hand out a raw
    // AudioDecodeWorker* that outlives the lock scope.
    std::shared_ptr<AudioDecodeWorker> findWorker(entt::entity e) const;

    // Shared discontinuity-seek core: compares expectedSample against the
    // worker's lastExpectedSample with a gap-scaled allowance, issues the
    // clamped seek, and restamps the state atomics. Called by the editor
    // update() and tickFromSnapshot — touches only worker atomics, safe
    // from either thread including wake overlap.
    static void steerAudioWorker(AudioDecodeWorker& w, int64_t expectedSample,
                                 int64_t nowNs, int rate, double tlFPS);

    Timeline*              m_timeline{nullptr};
    AudioEngine*           m_audioEngine{nullptr};
    PlaybackTimeAuthority* m_timeAuthority{nullptr};
    std::thread::id        m_editorThreadId;

    // Guards the m_workers map object (find / emplace / erase / iterate).
    // Same leaf-lock rules as DecodeSystem::m_workersMutex (issue #74):
    //  1. Never hold it across thread::join, AudioMixer register/unregister,
    //     Timeline calls, or decoder calls.
    //  2. Readers copy the shared_ptr out under a brief lock; atomic stores
    //     happen after unlock.
    //  3. Map mutation (create/retire/destroy/reap/shutdown) stays
    //     editor-thread-only.
    // Discontinuity-seek state lives on the worker itself
    // (AudioDecodeWorker::lastExpectedSample) since issue #74 — there is no
    // system-owned per-entity steering state anymore.
    mutable TracyLockable(std::mutex, m_workersMutex);
    std::unordered_map<entt::entity, std::shared_ptr<AudioDecodeWorker>> m_workers;
    // warmset::compute grace bookkeeping (editor-thread only, NOT covered by
    // m_workersMutex).
    std::unordered_map<entt::entity, int64_t> m_lastWarmNs;

    // Workers signaled to stop + unregistered from the mixer but not yet
    // joined (retire-and-reap teardown). Drained by reapRetiredWorkers() once
    // each thread sets `finished`, and unconditionally in shutdown/dtor.
    std::vector<std::pair<entt::entity, std::shared_ptr<AudioDecodeWorker>>> m_retiredWorkers;
};

} // namespace entity
