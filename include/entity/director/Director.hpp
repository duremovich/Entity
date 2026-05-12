#pragma once

#include <entt/entt.hpp>

#include <memory>

namespace entity {

class IRenderer;
class Timeline;
class ProjectManager;
class TranscodeManager;
class CommandDispatcher;
class AnimationSystem;
class GenerativeSystem;
class SceneState;
class PlaybackTimeAuthority;
class SectionScheduler;
class CaptureBroker;

// Director owns Phase D's "logical-side" subsystems -- the half of the engine
// that decides *what* to render this tick (timeline state, project state,
// per-clip math), as opposed to the Renderer half that decides *how* to put
// pixels on the GPU. Created by Engine, lives for Engine's lifetime.
//
// As of subtask 6, Director also owns `PlaybackTimeAuthority` (the
// Director half of the old PlaybackController) -- frame timing,
// clip-frame math, per-tick active-set computation. Engine continues
// to call into the owned subsystems directly via raw-pointer shortcuts.
// Subtask 8 turns the Director->Renderer per-tick state-snapshot into
// a bus message.
class Director {
public:
    // `renderer` is forwarded to ProjectManager::initialize so it can issue
    // GPU-bound work (allocating screen render targets) on project load.
    Director(entt::registry& registry,
             SceneState& sceneState,
             IRenderer* renderer);
    ~Director();

    Director(const Director&) = delete;
    Director& operator=(const Director&) = delete;
    Director(Director&&) = delete;
    Director& operator=(Director&&) = delete;

    // Owned subsystems.
    Timeline* getTimeline()                 noexcept { return m_timeline.get(); }
    ProjectManager* getProjectManager()     noexcept { return m_projectManager.get(); }
    TranscodeManager* getTranscodeManager() noexcept { return m_transcodeManager.get(); }
    CommandDispatcher* getCommandDispatcher() noexcept { return m_commandDispatcher.get(); }
    AnimationSystem* getAnimationSystem()   noexcept { return m_animationSystem.get(); }
    GenerativeSystem* getGenerativeSystem() noexcept { return m_generativeSystem.get(); }
    PlaybackTimeAuthority* getTimeAuthority() noexcept { return m_timeAuthority.get(); }
    SectionScheduler* getSectionScheduler()   noexcept { return m_sectionScheduler.get(); }
    CaptureBroker* getCaptureBroker()       noexcept { return m_captureBroker.get(); }

    const Timeline* getTimeline()                 const noexcept { return m_timeline.get(); }
    const ProjectManager* getProjectManager()     const noexcept { return m_projectManager.get(); }
    const TranscodeManager* getTranscodeManager() const noexcept { return m_transcodeManager.get(); }
    const CommandDispatcher* getCommandDispatcher() const noexcept { return m_commandDispatcher.get(); }
    const AnimationSystem* getAnimationSystem()   const noexcept { return m_animationSystem.get(); }
    const GenerativeSystem* getGenerativeSystem() const noexcept { return m_generativeSystem.get(); }
    const PlaybackTimeAuthority* getTimeAuthority() const noexcept { return m_timeAuthority.get(); }
    const SectionScheduler* getSectionScheduler() const noexcept { return m_sectionScheduler.get(); }
    const CaptureBroker* getCaptureBroker()       const noexcept { return m_captureBroker.get(); }

    // SceneState seam. Director writes the registry during its tick (clip
    // creation, keyframe evaluation, ProjectManager load). Subtask 8's
    // RenderFrame snapshot is built from this same registry.
    SceneState& sceneState() noexcept { return m_sceneState; }
    entt::registry& registry() noexcept { return m_registry; }

private:
    entt::registry& m_registry;
    SceneState&     m_sceneState;

    std::unique_ptr<Timeline>          m_timeline;
    std::unique_ptr<ProjectManager>    m_projectManager;
    std::unique_ptr<TranscodeManager>  m_transcodeManager;
    std::unique_ptr<CommandDispatcher> m_commandDispatcher;
    std::unique_ptr<AnimationSystem>   m_animationSystem;
    // Ticks GenerativeLayer-marked entities each editor frame. V1 just
    // advances per-kind sim counters; future commits add real per-kind
    // logic (Muncher gameplay, particles, ...). See systems/GenerativeSystem.hpp.
    std::unique_ptr<GenerativeSystem>  m_generativeSystem;
    // Owns frame timing + clip-frame math + per-tick active-set
    // computation (Phase D entry, subtask 6 -- replaces the Director
    // half of the old PlaybackController). Declared after Timeline +
    // ProjectManager so its construction can reference both.
    std::unique_ptr<PlaybackTimeAuthority> m_timeAuthority;
    // Phase B (sections + cue tags). Watches Timeline playback for section
    // break crossings and parks the playhead at each one. Engine ticks it
    // each main-loop update right after Timeline::update.
    std::unique_ptr<SectionScheduler>  m_sectionScheduler;
    // Phase D entry, subtask 7. Director-side park/resolve for the async
    // capture-command request/reply pattern. Engine wires its transport +
    // dispatcher pointers post-construction.
    std::unique_ptr<CaptureBroker>     m_captureBroker;
};

} // namespace entity
