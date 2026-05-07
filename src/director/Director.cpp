#include "entity/director/Director.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/SceneState.hpp"
#include "entity/director/CaptureBroker.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/SectionScheduler.hpp"
#include "entity/media/TranscodeManager.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/systems/AnimationSystem.hpp"
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
    , m_animationSystem(std::make_unique<AnimationSystem>())
    , m_timeAuthority(std::make_unique<PlaybackTimeAuthority>(registry, m_timeline.get()))
    , m_sectionScheduler(std::make_unique<SectionScheduler>(registry, m_timeline.get()))
    , m_captureBroker(std::make_unique<CaptureBroker>())
{
    // ProjectManager needs the Timeline + registry + renderer to honour
    // load/save calls (it allocates render-target slots for Screens at load
    // time). Mirrors what Engine::initialize used to do explicitly.
    m_projectManager->initialize(m_timeline.get(), &registry, renderer);

    // AnimationSystem reads keyframes against Timeline's current frame each
    // tick. Initialize against the registry now so its update() is a no-op
    // until clips with AnimatedProperties components show up.
    m_animationSystem->setTimeline(m_timeline.get());
    m_animationSystem->initialize(registry);

    // Wire ProjectManager into the time authority so the per-tick
    // active-set tuples carry the per-clip MediaBin OCIO override
    // (Phase C.12 #9). Done after ProjectManager::initialize so the
    // entry table is ready by the time the authority looks anything up.
    m_timeAuthority->setProjectManager(m_projectManager.get());

    // Capture broker resolves script-results via the dispatcher. Wire
    // the dispatcher now; the transport is wired by Engine post-init
    // (transport is owned by Engine and constructed after Director).
    m_captureBroker->setCommandDispatcher(m_commandDispatcher.get());
}

Director::~Director() {
    // Tear down AnimationSystem against the registry before its unique_ptr
    // runs. Mirrors System lifecycle contracts elsewhere in the codebase.
    if (m_animationSystem) {
        m_animationSystem->shutdown(m_registry);
    }
}

} // namespace entity
