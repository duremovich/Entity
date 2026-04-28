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
class SceneState;

// Director owns Phase D's "logical-side" subsystems -- the half of the engine
// that decides *what* to render this tick (timeline state, project state,
// per-clip math), as opposed to the Renderer half that decides *how* to put
// pixels on the GPU. Created by Engine, lives for Engine's lifetime.
//
// In this milestone (Phase D entry, subtask 4) Director just owns the
// subsystems and Engine continues to call into them directly via raw-pointer
// shortcuts. Subtask 6 will split PlaybackController into a Director-side
// PlaybackTimeAuthority and a Renderer-side PlaybackPresenter; subtask 8
// turns the Director->Renderer per-tick state-snapshot into a bus message.
//
// AnimationSystem is referenced here as a non-owning pointer set after
// registerSystem(); subtask 5 will replace the Engine m_systems vector with
// explicit named ownership and Director will take the unique_ptr.
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

    const Timeline* getTimeline()                 const noexcept { return m_timeline.get(); }
    const ProjectManager* getProjectManager()     const noexcept { return m_projectManager.get(); }
    const TranscodeManager* getTranscodeManager() const noexcept { return m_transcodeManager.get(); }
    const CommandDispatcher* getCommandDispatcher() const noexcept { return m_commandDispatcher.get(); }

    // Non-owning. Engine sets this after registering the system into its
    // m_systems vector. Subtask 5 promotes this to unique_ptr ownership.
    void setAnimationSystem(AnimationSystem* s) noexcept { m_animationSystem = s; }
    AnimationSystem* getAnimationSystem() noexcept { return m_animationSystem; }

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

    AnimationSystem* m_animationSystem{nullptr};
};

} // namespace entity
