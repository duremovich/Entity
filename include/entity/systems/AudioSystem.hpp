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
