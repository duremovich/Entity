#pragma once
#include "entity/systems/System.hpp"
#include "entity/audio/AudioDecodeWorker.hpp"
#include <entt/entt.hpp>
#include <memory>
#include <thread>
#include <unordered_map>

namespace entity {

class Timeline;
class AudioEngine;
class PlaybackTimeAuthority;

// Owns per-clip audio decode workers and drives them from the timeline.
// Editor-affinity with a show-thread fallback (ADR-0014): per-tick work
// writes only atomics; worker create/destroy is editor-thread-only.
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

    // Minimum ring fill (~43 ms @ 48 kHz) required before SeekSyncController
    // considers an audio worker primed.
    static constexpr size_t kAudioPrerollFrames = 2048;

private:
    void createWorker(entt::entity e, entt::registry& registry);
    void destroyWorker(entt::entity e);

    Timeline*              m_timeline{nullptr};
    AudioEngine*           m_audioEngine{nullptr};
    PlaybackTimeAuthority* m_timeAuthority{nullptr};
    std::thread::id        m_editorThreadId;

    std::unordered_map<entt::entity, std::shared_ptr<AudioDecodeWorker>> m_workers;
    // Per-entity last expected sample used by discontinuity-based seek detection.
    // Keyed same as m_workers; erased in destroyWorker.
    std::unordered_map<entt::entity, int64_t> m_lastExpectedSample;
};

} // namespace entity
