#include "entity/renderer/Renderer.hpp"

#include "entity/color/OcioManager.hpp"
#include "entity/core/SceneState.hpp"
#include "entity/media/DecodeBufferAllocators.hpp"
#include "entity/media/DecodeBufferPool.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/render/OutputManager.hpp"
#include "entity/render/UploadHeapBufferPool.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"
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

    // CPU-heap decode-buffer pool. Still constructed so CpuHeap fallback
    // paths in UploadHeapDecodeBufferAllocator have a pool to recycle into.
    // Constructed before FrameCache so it outlives the cache during shutdown --
    // the cache's eviction deleters call back into the pool via weak_ptr.
    m_decodeBufferPool = std::make_shared<DecodeBufferPool>();

    // Engine-global frame cache. Decode workers (spawned later by
    // DecodeSystem as clips are imported) push into it; PlaybackController
    // / PlaybackPresenter (subtask 6) reads via FrameLease.
    m_frameCache = std::make_unique<FrameCache>(frameCacheBytes);
    m_frameCache->setBufferPool(m_decodeBufferPool);

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
    //
    // Phase 5: wire DecodeSystem with an UploadHeapDecodeBufferAllocator.
    // Decode workers write pixels into D3D12 UPLOAD-heap buffers (write-combining
    // memory — fast for CPU writes, catastrophic for CPU reads). PlaybackPresenter
    // bypasses the CPU memcpy by recording CopyTextureRegion directly from the
    // buffer via copyUploadBufferToVideoTextureSlot. These two pieces (allocator
    // wiring + GPU-direct copy path) land together in Phase 5 — either alone
    // would regress perf (allocator alone → show-thread WC reads; GPU-direct
    // alone → no UploadHeap frames to copy).
    //
    // UploadHeapBufferPool requires the device, direct command queue, and show
    // fence from D3D12Renderer. getCommandQueue() / getShowFence() are
    // exposed for exactly this wiring (see D3D12Renderer.hpp:354-358).
    // Pool sizing: maxTotal must exceed the peak number of UploadHeap frames
    // simultaneously live in the FrameCache so acquire() hits the recycle bucket
    // instead of falling through to allocateFresh (CreateCommittedResource).
    // Peak = FrameCache budget / per-frame bytes. At 3 GiB and 8.3 MB per
    // 1080p RGBA8 frame, peak ≈ 370 frames. maxBuffersPerSize caps the per-size
    // recycle bucket — must also exceed peak so returning frames aren't dropped.
    // Set both to 512 for ample headroom (2× peak). Warmup cost: the first 370
    // acquire() calls each do one CreateCommittedResource; after that all
    // subsequent acquires hit the recycle bucket at O(1). Warmup happens on
    // decode worker threads and is bounded to the first few seconds after
    // project load.
    static constexpr size_t kPoolMax = 512;
    m_uploadHeapPool = std::make_shared<UploadHeapBufferPool>(
        m_d3d12Renderer->getDevice(),
        m_d3d12Renderer->getCommandQueue(),
        m_d3d12Renderer->getShowFence(),
        /*maxBuffersPerSize=*/kPoolMax,
        /*maxTotal=*/kPoolMax);
    // Wire pool into D3D12Renderer so copyUploadBufferToVideoTextureSlot can
    // call recordCopyDone after recording CopyTextureRegion.
    m_d3d12Renderer->setUploadHeapPool(m_uploadHeapPool.get());

    m_uploadHeapAllocator = std::make_unique<UploadHeapDecodeBufferAllocator>(m_uploadHeapPool);
    m_decodeSystem = std::make_unique<DecodeSystem>();
    m_decodeSystem->setFrameCache(m_frameCache.get());
    m_decodeSystem->setAllocator(m_uploadHeapAllocator.get());
    m_decodeSystem->initialize(m_registry);

    // Renderer-side half of the old PlaybackController (Phase D entry,
    // subtask 6). Holds backend + cache + decode-system references; the
    // Timeline pointer is wired by Engine after Director is built (it's
    // a Director-side state read).
    m_playbackPresenter = std::make_unique<PlaybackPresenter>(
        m_registry, m_d3d12Renderer.get(), m_frameCache.get());
    m_playbackPresenter->setDecodeSystem(m_decodeSystem.get());

    // CompositorSystem queries PlaybackPresenter for per-clip display state
    // (colour-space tags) on the show thread. The data lives there instead
    // of on VideoTexture so the show thread doesn't have to read the
    // registry — reads would race with the editor thread's
    // registry.destroy on clip delete (ADR-0014).
    m_compositorSystem->setPlaybackPresenter(m_playbackPresenter.get());

    m_initialized = true;
    return Result::Success;
}

void Renderer::shutdown() {
    if (!m_initialized) return;

    // Presenter holds raw refs into FrameCache + DecodeSystem; release
    // first so those are still alive while it goes away.
    m_playbackPresenter.reset();

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
    // Allocator holds a shared_ptr into m_uploadHeapPool; reset before the
    // pool. Decode workers (joined above in m_decodeSystem->shutdown) are done,
    // so no live reference to the allocator remains.
    m_uploadHeapAllocator.reset();
    // Upload-heap pool: stop the recycle worker (which fence-waits on buffers
    // that were still in flight) then release. Must happen after allocator and
    // after decode workers (and therefore after FrameCache, which may still
    // hold UploadHeapBuffer shared_ptrs via FrameLease until frame.reset()
    // above clears them). Also clear D3D12Renderer's raw pointer before the
    // pool destructs so D3D12Renderer can't dereference a dangling pointer
    // during its own shutdown.
    if (m_d3d12Renderer) {
        m_d3d12Renderer->setUploadHeapPool(nullptr);
    }
    m_uploadHeapPool.reset();
    // CPU heap pool. Kept alive through m_frameCache.reset() for the same
    // reason: eviction deleters may still weak_ptr.lock() into it.
    m_decodeBufferPool.reset();

    // Backend last.
    m_d3d12Renderer.reset();

    m_initialized = false;
}

Result Renderer::resize(uint32_t width, uint32_t height) {
    if (!m_d3d12Renderer) return Result::Failure;
    return m_d3d12Renderer->resize(width, height);
}

} // namespace entity
