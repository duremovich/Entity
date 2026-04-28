#include "entity/director/Director.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/SceneState.hpp"
#include "entity/media/TranscodeManager.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/timeline/Timeline.hpp"

namespace entity {

Director::Director(entt::registry& registry,
                   SceneState& sceneState,
                   IRenderer* renderer)
    : m_registry(registry)
    , m_sceneState(sceneState)
    , m_timeline(std::make_unique<Timeline>(registry))
    , m_projectManager(std::make_unique<ProjectManager>())
    , m_transcodeManager(std::make_unique<TranscodeManager>())
    , m_commandDispatcher(std::make_unique<CommandDispatcher>())
{
    // ProjectManager needs the Timeline + registry + renderer to honour
    // load/save calls (it allocates render-target slots for Screens at load
    // time). Mirrors what Engine::initialize used to do explicitly.
    m_projectManager->initialize(m_timeline.get(), &registry, renderer);
}

Director::~Director() = default;

} // namespace entity
