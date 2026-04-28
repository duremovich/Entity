#include "entity/renderer/Renderer.hpp"

#include "entity/color/OcioManager.hpp"
#include "entity/core/SceneState.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/render/OutputManager.hpp"
#include "entity/systems/CompositorSystem.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/systems/TestSystem.hpp"

#include <iostream>

namespace entity {

Renderer::Renderer(entt::registry& registry, SceneState& sceneState)
    : m_registry(registry)
    , m_sceneState(sceneState)
{}

Renderer::~Renderer() {
    shutdown();
}

IRenderer* Renderer::getRenderer() const noexcept {
    // Upcast through the IRenderer base. Callers see only the interface;
    // D3D12 types stay quarantined to entity-render.
    return m_d3d12Renderer.get();
}

Result Renderer::initialize(GLFWwindow* window,
                            uint32_t width,
                            uint32_t height,
                            std::size_t frameCacheBytes) {
    if (m_initialized) {
        std::cerr << "Renderer already initialized!" << std::endl;
        return Result::Failure;
    }

    // GPU backend first -- everything else hangs off the device.
    m_d3d12Renderer = std::make_unique<D3D12Renderer>();
    if (Result r = m_d3d12Renderer->initialize(window, width, height); r != Result::Success) {
        std::cerr << "Renderer: failed to initialize D3D12 backend!" << std::endl;
        return r;
    }

    // Output manager owns physical-display swap chains; it holds slot IDs
    // into D3D12Renderer and so must be torn down before the backend.
    m_outputManager = std::make_unique<OutputManager>(m_d3d12Renderer.get(), m_registry);
    if (Result r = m_outputManager->initialize(); r != Result::Success) {
        std::cerr << "Renderer: failed to initialize OutputManager!" << std::endl;
        return r;
    }

    // Engine-global frame cache. Decode workers (spawned later by
    // DecodeSystem as clips are imported) push into it; PlaybackController
    // / PlaybackPresenter (subtask 6) reads via FrameLease.
    m_frameCache = std::make_unique<FrameCache>(frameCacheBytes);

    // OCIO config + processor cache. Empty path = bundled ACES Studio
    // Config 1.3 via Config::CreateFromBuiltinConfig (Phase C.12 #1).
    m_ocioManager = std::make_unique<OcioManager>();
    if (!m_ocioManager->initialize()) {
        std::cerr << "Renderer: OCIO config init fell back to identity -- color "
                     "management will be a no-op until a valid config is loaded.\n";
    }
    // Wire OCIO into the D3D12 backend so per-clip composite + per-output
    // mapping_surface PSOs are baked from the active config (Phase C.12 #5).
    m_d3d12Renderer->setOcioManager(m_ocioManager.get());

    // CompositorSystem composites the per-screen render targets each
    // frame. Engine sets the Timeline pointer after Director is built.
    m_compositorSystem = std::make_unique<CompositorSystem>(m_d3d12Renderer.get());
    m_compositorSystem->initialize(m_registry);

    // TestSystem stays around as a heartbeat / lifecycle check; cheap, and
    // the Phase D split is not the right time to delete it.
    m_testSystem = std::make_unique<TestSystem>();
    m_testSystem->initialize(m_registry);

    // DecodeSystem owns per-clip decode worker threads. They spawn lazily
    // via Engine::onClipCreated; setFrameCache wires the producer half of
    // the engine-global cache.
    m_decodeSystem = std::make_unique<DecodeSystem>();
    m_decodeSystem->setFrameCache(m_frameCache.get());
    m_decodeSystem->initialize(m_registry);

    m_initialized = true;
    return Result::Success;
}

void Renderer::shutdown() {
    if (!m_initialized) return;

    // Systems first -- DecodeSystem joins its worker threads in shutdown,
    // and they may still be reading FrameCache / using the device. Order:
    // decode (joins threads) -> test (no-op) -> compositor (releases
    // pipeline state).
    if (m_decodeSystem)     { m_decodeSystem->shutdown(m_registry);     m_decodeSystem.reset(); }
    if (m_testSystem)       { m_testSystem->shutdown(m_registry);       m_testSystem.reset(); }
    if (m_compositorSystem) { m_compositorSystem->shutdown(m_registry); m_compositorSystem.reset(); }

    // OutputManager holds swap-chain slot IDs into D3D12Renderer; release
    // before the backend goes away.
    if (m_outputManager) {
        m_outputManager->shutdown();
        m_outputManager.reset();
    }

    // OCIO + cache outlive the systems on purpose -- the cache may still
    // be reachable via FrameLease handles held by callers above. By the
    // time we hit this line, all consumers (PlaybackController, decode
    // workers) are already gone.
    m_ocioManager.reset();
    m_frameCache.reset();

    // Backend last.
    m_d3d12Renderer.reset();

    m_initialized = false;
}

Result Renderer::resize(uint32_t width, uint32_t height) {
    if (!m_d3d12Renderer) return Result::Failure;
    return m_d3d12Renderer->resize(width, height);
}

} // namespace entity
