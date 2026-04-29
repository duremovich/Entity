#pragma once

#include "entity/core/Types.hpp"
#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

struct GLFWwindow;

namespace entity {

class IRenderer;
class D3D12Renderer;
class OutputManager;
class FrameCache;
class DecodeBufferPool;
class OcioManager;
class CompositorSystem;
class DecodeSystem;
class TestSystem;
class SceneState;
class PlaybackPresenter;

// Renderer is one half of the Phase D entry decomposition. It owns the
// GPU-side subsystems -- everything that decides *how* to put pixels on a
// surface, as opposed to the Director half that decides *what* to render
// this tick (timeline state, project state, per-clip math).
//
// Named "Renderer" (not "RendererService") even though `IRenderer` is the
// GPU backend interface -- the two never collide at the call sites because
// IRenderer is always referenced via pointer / interface, while Renderer is
// referenced as the owning service. Engine's existing raw shortcut
// `m_renderer` (formerly `unique_ptr<D3D12Renderer>`) becomes a raw
// `IRenderer*` pointing at the service's owned backend, keeping all
// existing `m_renderer->beginFrame()` etc. call sites unchanged.
//
// As of subtask 6, Renderer also owns `PlaybackPresenter` (the
// Renderer half of the old PlaybackController) -- consumes
// `ActiveClip` tuples computed by the Director-side PlaybackTimeAuthority
// each tick. Engine still wires the cross-side glue with direct method
// calls; subtask 8 turns the Director->Renderer per-tick state-snapshot
// into a bus message (`RenderFrame`) and Engine::run() collapses to
// director.tick() + renderer.tick().
class Renderer {
public:
    Renderer(entt::registry& registry, SceneState& sceneState);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Two-phase init mirrors D3D12Renderer's contract -- the GPU backend
    // can fail (device creation, swap chain, shader compile) and the editor
    // surfaces that as a Result the caller can act on.
    //
    // Order inside: D3D12Renderer -> OutputManager -> FrameCache ->
    // OcioManager -> wire OcioManager into D3D12Renderer -> CompositorSystem
    // -> TestSystem -> DecodeSystem (decode workers don't run yet -- they
    // spawn lazily as clips are imported) -> PlaybackPresenter (subtask 6).
    //
    // Timeline pointers on CompositorSystem / DecodeSystem / PlaybackPresenter
    // are NOT set here -- Engine wires them after Director is constructed
    // (subtask 8 moves that wiring through the bus instead).
    Result initialize(GLFWwindow* window,
                      uint32_t width,
                      uint32_t height,
                      std::size_t frameCacheBytes);

    // Tear down in reverse: systems first (decode workers join here), then
    // OutputManager (which holds swap-chain slot IDs into D3D12Renderer),
    // then OcioManager + FrameCache, then D3D12Renderer last. Idempotent.
    void shutdown();

    Result resize(uint32_t width, uint32_t height);

    // Owned subsystems. Everything is created in initialize() and lives
    // until shutdown(); accessors return nullptr before initialize() and
    // after shutdown().
    IRenderer*         getRenderer() const noexcept;
    D3D12Renderer*     getD3D12Renderer() const noexcept    { return m_d3d12Renderer.get(); }
    OutputManager*     getOutputManager() const noexcept    { return m_outputManager.get(); }
    FrameCache*        getFrameCache() const noexcept       { return m_frameCache.get(); }
    DecodeBufferPool*  getDecodeBufferPool() const noexcept { return m_decodeBufferPool.get(); }
    OcioManager*       getOcioManager() const noexcept      { return m_ocioManager.get(); }
    CompositorSystem*  getCompositorSystem() const noexcept { return m_compositorSystem.get(); }
    DecodeSystem*      getDecodeSystem() const noexcept     { return m_decodeSystem.get(); }
    TestSystem*        getTestSystem() const noexcept       { return m_testSystem.get(); }
    PlaybackPresenter* getPlaybackPresenter() const noexcept { return m_playbackPresenter.get(); }

    // SceneState seam. Renderer reads the registry during its tick (clip
    // VideoTexture state, mapping surfaces, screens). Subtask 8's
    // RenderFrame snapshot will replace most direct registry reads with
    // bus-delivered state.
    SceneState&       sceneState() noexcept { return m_sceneState; }
    entt::registry&   registry() noexcept   { return m_registry; }

private:
    entt::registry& m_registry;
    SceneState&     m_sceneState;

    // Declared in destruction order (reverse of construction): systems
    // tear down first, then OutputManager, then OCIO + cache, then the
    // backend. unique_ptr destructors run in reverse-declaration order, so
    // the declaration order here IS the construction order.
    std::unique_ptr<D3D12Renderer>     m_d3d12Renderer;
    std::unique_ptr<OutputManager>     m_outputManager;
    // Pool sits BEFORE FrameCache in declaration order so it constructs
    // first and destructs LAST -- FrameCache's eviction deleters need the
    // pool alive on tear-down (they do `weak_ptr.lock()` to recycle the
    // freed pixel buffer; if the pool is gone they fall through to `delete`,
    // safe but loses the recycle on the final shutdown wave).
    std::shared_ptr<DecodeBufferPool>  m_decodeBufferPool;
    std::unique_ptr<FrameCache>        m_frameCache;
    std::unique_ptr<OcioManager>       m_ocioManager;
    std::unique_ptr<CompositorSystem>  m_compositorSystem;
    std::unique_ptr<TestSystem>        m_testSystem;
    std::unique_ptr<DecodeSystem>      m_decodeSystem;
    // Renderer-side half of the old PlaybackController (Phase D entry,
    // subtask 6). Constructed in initialize() after FrameCache + the
    // backend exist; takes the DecodeSystem pointer right after that
    // system is up. Tear-down order is reverse-declaration -- presenter
    // releases its references before DecodeSystem joins worker threads.
    std::unique_ptr<PlaybackPresenter> m_playbackPresenter;

    bool m_initialized{false};
};

} // namespace entity
