/**
 * D3D12Renderer Implementation
 *
 * Basic D3D12 renderer that initializes the device and clears to a color.
 */

#include "entity/render/D3D12Renderer.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/render/D3D12Device.hpp"
#include "entity/render/DescriptorHeapLayout.hpp"
#include "entity/render/TextureUploader.hpp"
#include "entity/render/UploadHeapBufferPool.hpp"
#include "entity/render/MeshUploader.hpp"
#include "entity/color/OcioManager.hpp"
#include "entity/color/OcioGpuProcessor.hpp"
#include "entity/core/CrashLogger.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_dx12.h>

// stb_image_write for PNG export
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace entity {

// 2026-05-23 — fence-wait timeout configurable via env var
// `ENTITY_FENCE_TIMEOUT_MS`. Default 5000. Profiler attachment (Tracy) and
// heavy first-frame work on multi-output 4K H.264 projects can blow past a
// smaller budget once, false-positive as device-lost. Bump to 8000+ when
// running under Tracy or against a heavyweight project; leave at default in
// production. One-time env read; zero hot-path overhead.
//
// 2026-06-11 — raised the default 2000 -> 5000. The old 2000 ms sat inside
// the ~2 s driver TDR window: a genuine GPU hang would trip our fence
// timeout *before* the driver declared removal, so GetDeviceRemovedReason()
// still returned S_OK (0x0) and the device-lost report carried a misleading
// reason. 5000 ms lets the driver declare removal first; the wait sites pair
// this with a re-query-after-grace (see the WAIT_TIMEOUT handlers) so a real
// TDR is captured with its actual reason. See docs/perf/device-hung-2026-06.
namespace {
DWORD fenceWaitTimeoutMs() {
    static const DWORD ms = []() -> DWORD {
        const char* v = std::getenv("ENTITY_FENCE_TIMEOUT_MS");
        if (v && *v) {
            char* end = nullptr;
            long parsed = std::strtol(v, &end, 10);
            if (end != v && parsed > 0) return static_cast<DWORD>(parsed);
        }
        return 5000;
    }();
    return ms;
}
} // namespace

// Thread-local active command list and descriptor-heap cache.
// beginShowFrame (show thread) and beginEditorFrame (editor thread) each write
// their own copy; all draw methods read their thread's copy. This avoids the
// data race that would occur if both threads wrote a shared member concurrently.
thread_local ID3D12GraphicsCommandList* tl_activeCmdList{nullptr};
thread_local ID3D12DescriptorHeap*      tl_currentDescriptorHeap{nullptr};

// Show GPU zone spans beginShowFrame → endShowFrame so it covers all GPU work
// recorded between the allocator reset and Close(). Heap-allocated so the
// non-default-constructible D3D12ZoneScope can live across the call boundary.
// Show-thread only — never read or written by the editor thread.
#if defined(ENTITY_ENABLE_TRACY) && defined(TRACY_ENABLE)
static constexpr tracy::SourceLocationData kShowGpuSrcLoc{
    "Show GPU", "D3D12Renderer::showFrame", __FILE__, __LINE__, 0 };
thread_local tracy::D3D12ZoneScope* tl_showGpuZone{nullptr};
#endif

D3D12Renderer::D3D12Renderer()
    : m_currentBackBufferIndex(0)
    , m_rtvDescriptorSize(0)
    , m_width(0)
    , m_height(0)
    , m_initialized(false)
    , m_fenceEvent(nullptr)
    , m_showFenceEvent(nullptr)
    , m_constantBufferData(nullptr)
{
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_editorFenceValues[i] = 1;
        m_showFenceValues[i]   = 1;
    }
    m_vertexBufferView = {};
}

D3D12Renderer::~D3D12Renderer() {
    shutdown();
}

Result D3D12Renderer::initialize(GLFWwindow* window, uint32_t width, uint32_t height) {
    if (m_initialized) {
        std::cerr << "D3D12Renderer already initialized!" << std::endl;
        return Result::Failure;
    }

    m_width = width;
    m_height = height;

    std::cout << "Initializing D3D12 renderer..." << std::endl;

    // Create D3D12 device + command queue (owned by D3D12Device wrapper)
    m_gpu = std::make_unique<D3D12Device>();
#if defined(_DEBUG)
    constexpr bool kEnableDebugLayer = true;
#else
    constexpr bool kEnableDebugLayer = false;
#endif
    if (m_gpu->initialize(kEnableDebugLayer) != Result::Success) {
        return Result::Failure;
    }

    // Pass GPU identity to CrashLogger so device-lost context.json has adapter info.
    CrashLogger::setAdapterDesc(m_gpu->adapterDesc());

    m_tracyD3D12Ctx = TracyD3D12Context(m_gpu->device(), m_gpu->commandQueue());
    TracyD3D12ContextName(m_tracyD3D12Ctx, "DirectQ", 7);

    // Get Win32 window handle from GLFW
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) {
        std::cerr << "Failed to get Win32 window handle!" << std::endl;
        return Result::Failure;
    }

    // Create swap chain
    if (createSwapChain(hwnd, width, height) != Result::Success) {
        std::cerr << "Failed to create swap chain!" << std::endl;
        return Result::Failure;
    }

    // Create render target views
    if (createRenderTargetViews() != Result::Success) {
        std::cerr << "Failed to create render target views!" << std::endl;
        return Result::Failure;
    }

    // Create command allocators
    if (createCommandAllocators() != Result::Success) {
        std::cerr << "Failed to create command allocators!" << std::endl;
        return Result::Failure;
    }

    // Create command list
    if (createCommandList() != Result::Success) {
        std::cerr << "Failed to create command list!" << std::endl;
        return Result::Failure;
    }

    // Phase C.11: COPY-queue command allocators + list for async uploads
    if (createCopyCommandList() != Result::Success) {
        std::cerr << "Failed to create copy command list!" << std::endl;
        return Result::Failure;
    }

    // Create fence for synchronization
    if (createFence() != Result::Success) {
        std::cerr << "Failed to create fence!" << std::endl;
        return Result::Failure;
    }

    // Create rendering pipeline
    if (createRootSignature() != Result::Success) {
        std::cerr << "Failed to create root signature!" << std::endl;
        return Result::Failure;
    }

    if (createPipelineState() != Result::Success) {
        std::cerr << "Failed to create pipeline state!" << std::endl;
        return Result::Failure;
    }

    if (createVertexBuffer() != Result::Success) {
        std::cerr << "Failed to create vertex buffer!" << std::endl;
        return Result::Failure;
    }

    if (createConstantBuffer() != Result::Success) {
        std::cerr << "Failed to create constant buffer!" << std::endl;
        return Result::Failure;
    }

    // Initialize ImGui (creates the shader-visible SRV heap we'll share with
    // TextureUploader for video texture slots)
    if (initializeImGui(window) != Result::Success) {
        std::cerr << "Failed to initialize ImGui!" << std::endl;
        return Result::Failure;
    }

    // Video texture pool — depends on the SRV heap created by initializeImGui
    m_textureUploader = std::make_unique<TextureUploader>();
    if (m_textureUploader->initialize(m_gpu->device(), m_imguiSrvHeap.Get(), m_srvDescriptorSize) != Result::Success) {
        std::cerr << "Failed to initialize TextureUploader!" << std::endl;
        return Result::Failure;
    }

    // Mesh vertex/index buffer pool
    m_meshUploader = std::make_unique<MeshUploader>();
    if (!m_meshUploader->initialize(m_gpu->device())) {
        std::cerr << "Failed to initialize MeshUploader!" << std::endl;
        m_meshUploader.reset();
        // Non-fatal: mesh upload will be skipped, but 3D preview still works without meshes.
    } else {
        // Eagerly upload the shared default 16:9 screen quad so flat-screen-only
        // projects don't hit the bootstrap deadlock where ensureStageTarget never
        // fires (renderMeshes early-exits when all slots are UINT32_MAX).
        MeshData defaultMesh = createDefaultScreenMesh();
        m_defaultScreenMeshSlot = uploadMeshImmediate(defaultMesh.vertices, defaultMesh.indices);
        if (m_defaultScreenMeshSlot == UINT32_MAX) {
            std::cerr << "[D3D12Renderer] Failed to upload default screen mesh during init.\n";
        }
    }

    // Create textured rendering pipeline (for multi-layer compositing)
    if (createTexturedRootSignature() != Result::Success) {
        std::cerr << "Failed to create textured root signature!" << std::endl;
        return Result::Failure;
    }

    if (createTexturedPipelineState() != Result::Success) {
        std::cerr << "Failed to create textured pipeline state!" << std::endl;
        return Result::Failure;
    }

    if (createBlendRootSignature() != Result::Success) {
        std::cerr << "Failed to create blend root signature!" << std::endl;
        return Result::Failure;
    }

    if (createBlendPipelineState() != Result::Success) {
        std::cerr << "Failed to create blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    // Create mapping surface rendering pipeline (for projection mapping)
    if (createMappingSurfaceRootSignature() != Result::Success) {
        std::cerr << "Failed to create mapping surface root signature!" << std::endl;
        return Result::Failure;
    }

    if (createMappingSurfacePipelineState() != Result::Success) {
        std::cerr << "Failed to create mapping surface pipeline state!" << std::endl;
        return Result::Failure;
    }

    if (createMappingSurfaceVertexBuffer() != Result::Success) {
        std::cerr << "Failed to create mapping surface vertex buffer!" << std::endl;
        return Result::Failure;
    }

    if (createMappingSurfaceConstantBuffer() != Result::Success) {
        std::cerr << "Failed to create mapping surface constant buffer!" << std::endl;
        return Result::Failure;
    }

    // Per-layer effect chain (issue #54). Shared root signature + shared VS
    // blob + per-frame cbuffer ring. Per-kind PSOs are loaded lazily in
    // drawEffectPass via the EffectKindRegistry pointer set by Engine.
    if (createEffectRootSignature() != Result::Success) {
        std::cerr << "Failed to create effect root signature!" << std::endl;
        return Result::Failure;
    }
    if (loadCompiledShader(L"shaders/effect_vs.cso", &m_effectVsBlob) != Result::Success) {
        std::cerr << "Failed to load effect_vs.cso!" << std::endl;
        return Result::Failure;
    }
    if (createEffectConstantBufferRing() != Result::Success) {
        std::cerr << "Failed to create effect constant buffer ring!" << std::endl;
        return Result::Failure;
    }

    // Phase C.12 #3: tone-mapping pass (FP16 compose → UNORM8 capture) used
    // by readComposeTargetPixels / captureComposeTargetToPNG so the goldens
    // hash a portable, machine-independent UNORM8 image. The capture
    // resource is allocated lazily on first capture; only the PSO needs to
    // exist up front.
    if (!createCaptureRootSignatureAndPSO()) {
        std::cerr << "Failed to create capture pipeline state!" << std::endl;
        return Result::Failure;
    }

    // #75: compose targets are created at runtime on the show thread while
    // the editor thread reads the vector. Reserving the hard cap up front
    // (createComposeTarget enforces MAX_COMPOSE_TARGETS) means emplace_back
    // never reallocates, so element addresses — and the SRV handles the
    // editor holds into them — stay valid for the renderer's lifetime.
    m_composeTargets.reserve(IRenderer::MAX_COMPOSE_TARGETS);

    m_initialized = true;
    std::cout << "D3D12 renderer initialized successfully!" << std::endl;

    return Result::Success;
}

void D3D12Renderer::shutdown() {
    if (!m_initialized) {
        return;
    }

    std::cout << "Shutting down D3D12 renderer..." << std::endl;

    // Drain the GPU before releasing any resources. This must happen before
    // shutdownImGui(), which releases ImGui's D3D12 descriptors — releasing
    // them with frames still in flight intermittently hung the shutdown
    // fence wait on WARP.
    //
    // Skip the drain only on a CONFIRMED device removal (reason != S_OK): the
    // fences never signal, so waitForGpu() would burn a full
    // fenceWaitTimeoutMs() per drain (and, before the drains were bounded, hung
    // forever — observed: killed after 30 min); the GPU is gone, nothing to
    // wait for, and releasing resources without draining is safe.
    //
    // Gate on the reason, NOT the bare m_deviceLost flag. m_deviceLost can latch
    // spuriously (heavy stall trips a fence timeout while GetDeviceRemovedReason
    // still returns S_OK even after the grace re-query — see fenceWaitTimeoutMs).
    // On a spurious latch the device is LIVE and may still be executing the last
    // show frame, so we MUST still drain — skipping would release GPU-referenced
    // resources (shutdownImGui, CB unmaps, uploader teardown) out from under the
    // GPU (teardown UAF). The drains are bounded, so a genuinely-hung-but-S_OK
    // case still can't hang shutdown.
    const bool confirmedRemoval =
        m_deviceLost.load(std::memory_order_acquire) &&
        m_deviceLostReason.load(std::memory_order_acquire) != S_OK;
    if (!confirmedRemoval) {
        waitForGpu();
    }

    shutdownImGui();

    if (m_constantBuffer && m_constantBufferData) {
        m_constantBuffer->Unmap(0, nullptr);
        m_constantBufferData = nullptr;
    }

    if (m_mappingSurfaceConstantRing && m_mappingSurfaceConstantRingMapped) {
        m_mappingSurfaceConstantRing->Unmap(0, nullptr);
        m_mappingSurfaceConstantRingMapped = nullptr;
    }

    if (m_effectCbufferRing && m_effectCbufferRingMapped) {
        m_effectCbufferRing->Unmap(0, nullptr);
        m_effectCbufferRingMapped = nullptr;
    }

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    if (m_showFenceEvent) {
        CloseHandle(m_showFenceEvent);
        m_showFenceEvent = nullptr;
    }

    if (m_textureUploader) {
        m_textureUploader->shutdown();
        m_textureUploader.reset();
    }

    if (m_meshUploader) {
        // Drain any pending deferred frees — GPU is idle after waitForGpu() above.
        m_meshUploader->onFrameBegin(UINT64_MAX);
        m_meshUploader->shutdown();
        m_meshUploader.reset();
    }

    m_videoUploadBuffer.Reset();

    m_composeTargetCount.store(0, std::memory_order_release);
    for (auto& target : m_composeTargets) {
        for (uint32_t sub = 0; sub < ComposeTarget::TRIPLE; ++sub) {
            target.resources[sub].Reset();
            target.rtvHeaps[sub].Reset();
        }
    }
    m_composeTargets.clear();

    for (auto& ow : m_outputWindows) {
        if (!ow.active) continue;
        for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
            ow.renderTargets[i].Reset();
        }
        ow.rtvHeap.Reset();
        ow.swapChain.Reset();
        if (ow.window) {
            glfwDestroyWindow(ow.window);
            ow.window = nullptr;
        }
        ow.active = false;
    }
    m_outputWindows.clear();
    m_dxgiFactory.Reset();

    // Release textured pipeline objects
    m_texturedPipelineState.Reset();
    m_texturedRootSignature.Reset();

    // Release mapping surface pipeline objects
    m_mappingSurfacePipelineState.Reset();
    m_mappingSurfaceRootSignature.Reset();
    m_mappingSurfaceVertexBuffer.Reset();
    m_mappingSurfaceConstantRing.Reset();

    // Effect chain resources (issue #54)
    m_effectPsoCache.clear();
    m_effectRootSignature.Reset();
    m_effectVsBlob.Reset();
    m_effectCbufferRing.Reset();

    // Stage 3D render target resources
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (m_stageTarget.frameCBMapped[i] && m_stageTarget.frameCB[i]) {
            m_stageTarget.frameCB[i]->Unmap(0, nullptr);
            m_stageTarget.frameCBMapped[i] = nullptr;
        }
        m_stageTarget.frameCB[i].Reset();
        m_stageTarget.color[i].Reset();
        m_stageTarget.depth[i].Reset();
    }
    m_stageTarget.rtvHeap.Reset();
    m_stageTarget.dsvHeap.Reset();
    m_stageTarget.ready = false;

    // Stage 3D straight-alpha sibling + stage PSOs
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_stageTargetStraight.color[i].Reset();
    }
    m_stageTargetStraight.rtvHeap.Reset();
    m_stageTargetStraight.ready = false;
    m_stageMeshPipelineState.Reset();
    m_stageMeshPipelineStateTransparent.Reset();
    m_stageMeshRootSignature.Reset();
    m_stageDePremulPipelineState.Reset();
    m_stageDePremulRootSignature.Reset();

    // Phase C.12 #3 capture-pass resources
    m_capturePipelineState.Reset();
    m_captureRootSignature.Reset();
    m_captureResource.Reset();
    m_captureRtvHeap.Reset();

    // Release rendering pipeline objects
    m_cbvHeap.Reset();
    m_constantBuffer.Reset();
    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();

    // Release all COM objects (ComPtr handles this automatically)
    m_editorCmdList.Reset();
    m_showCmdList.Reset();
    m_copyCommandList.Reset();
    m_editorCopyCommandList.Reset();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_editorAllocators[i].Reset();
        m_showAllocators[i].Reset();
        m_copyCommandAllocators[i].Reset();
        m_editorCopyCommandAllocators[i].Reset();
        m_renderTargets[i].Reset();
    }
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_editorFence.Reset();
    m_showFence.Reset();
    m_uploadFence.Reset();
    // Destroy Tracy GPU context before releasing the device + queue.
    // GPU must be idle (waitForGpu called above) before destroy.
    TracyD3D12Destroy(m_tracyD3D12Ctx);
    m_tracyD3D12Ctx = nullptr;

    // Release the device + queue last (after all resources allocated from
    // them have been released above).
    if (m_gpu) {
        m_gpu->shutdown();
        m_gpu.reset();
    }

    m_initialized = false;
}

Result D3D12Renderer::resize(uint32_t width, uint32_t height) {
    if (!m_initialized) {
        return Result::Failure;
    }

    // Wait for GPU to finish
    waitForGpu();

    // Release render targets
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_renderTargets[i].Reset();
    }

    // Resize swap chain buffers
    HRESULT hr = m_swapChain->ResizeBuffers(
        FRAME_COUNT,
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        0
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to resize swap chain buffers!" << std::endl;
        return Result::Failure;
    }

    m_width = width;
    m_height = height;

    // Recreate render target views
    return createRenderTargetViews();
}

// ---------------------------------------------------------------------------
// Stage 1 A/B-list split:
//   Show frame  = copy uploads + compositor + output draws + output Present
//   Editor frame = back-buffer setup + ImGui + editor Present
// Both still run sequentially on the main thread in Stage 1.
// Stage 3 moves showXxxFrame onto a separate thread.
// ---------------------------------------------------------------------------

void D3D12Renderer::beginShowFrame() {
    ZoneScopedN("beginShowFrame");

    // Show thread rotates its own ring slot independently — never calls
    // GetCurrentBackBufferIndex(), which is not thread-safe to call
    // concurrently with the editor thread on the same IDXGISwapChain3.
    m_showBackBufferIndex = (m_showBackBufferIndex + 1) % FRAME_COUNT;

    // Wait for the show fence for this ring slot before resetting its allocator.
    // m_showFenceValues[slot] holds the NEXT value to signal (pending), so the
    // last signaled value for this slot is showFenceValues[slot]-1. On the very
    // first frame that value is 0 which equals the fence's initial completed
    // value — so no wait occurs. After each endShowFrame the pending value
    // advances by 1, and the gate fires only if the previous submission for this
    // slot is still in-flight (safe even at 120 Hz show / 60 Hz editor).
    // Stage 3: show thread uses m_showFenceEvent; editor uses m_fenceEvent.
    // Never share — concurrent SetEventOnCompletion on one handle is UB.
    if (m_showFence && m_showFenceValues[m_showBackBufferIndex] > 0) {
        const uint64_t lastSignaled = m_showFenceValues[m_showBackBufferIndex] - 1;
        if (m_showFence->GetCompletedValue() < lastSignaled) {
            ZoneScopedNC("GPU fence wait", 0xCC4444);
            m_showFence->SetEventOnCompletion(lastSignaled, m_showFenceEvent);
            DWORD result = WaitForSingleObject(m_showFenceEvent, fenceWaitTimeoutMs());
            if (result == WAIT_TIMEOUT || result == WAIT_FAILED) {
                HRESULT removedReason = removedReasonAfterTimeout();
                handleDeviceLost(removedReason, "beginShowFrame show fence wait timeout");
                return;
            }
        }
    }

    // Drain deferred video texture slot frees. The editor thread's
    // on_destroy<VideoTexture> observer enqueues slots into
    // m_deferredVideoTextureFrees. We drain here, after the show fence wait,
    // so the GPU is guaranteed not to be sampling those slots' textures.
    {
        std::vector<uint32_t> toFree;
        {
            std::lock_guard<std::mutex> lk(m_deferredVideoTextureFreesMutex);
            toFree.swap(m_deferredVideoTextureFrees);
        }
        if (m_textureUploader) {
            for (uint32_t slot : toFree) {
                m_textureUploader->freeSlot(slot);
                std::cout << "Freed video texture slot " << slot
                          << " (deferred)" << std::endl;
            }
        }
    }

    // CRIT-04: reset the per-frame mapping-surface CB ring cursor. Fence sync
    // above has ensured the GPU is done with this frame's show region.
    m_mappingSurfaceDrawIndex = 0;
    m_mappingSurfaceOverflowed = false;

    // Issue #54: same pattern for the per-layer effect chain CB ring.
    m_effectDrawIndex   = 0;
    m_effectOverflowed  = false;

    // Reset show-side allocator + command list
    m_showAllocators[m_showBackBufferIndex]->Reset();
    m_showCmdList->Reset(m_showAllocators[m_showBackBufferIndex].Get(), nullptr);

    // Phase C.11: reset the COPY queue's allocator + list for this frame.
    m_copyCommandAllocators[m_showBackBufferIndex]->Reset();
    m_copyCommandList->Reset(m_copyCommandAllocators[m_showBackBufferIndex].Get(), nullptr);
    m_uploadsRecordedThisFrame = false;

    // Reset descriptor heap cache (command list state is reset)
    tl_currentDescriptorHeap = nullptr;

    // Route all draw calls to the show command list
    tl_activeCmdList = m_showCmdList.Get();

    // Tracy: advance the D3D12 GPU frame counter after the fence wait and
    // allocator reset, so Tracy knows the query heap is safe to reuse.
    TracyD3D12NewFrame(m_tracyD3D12Ctx);

    // Open a GPU zone spanning the entire show command list recording window.
    // Closed in endShowFrame() just before Close() so the destructor injects
    // the end-timestamp query into the same open command list.
#if defined(ENTITY_ENABLE_TRACY) && defined(TRACY_ENABLE)
    tl_showGpuZone = new tracy::D3D12ZoneScope(
        m_tracyD3D12Ctx, m_showCmdList.Get(), &kShowGpuSrcLoc, true);
#endif
}

void D3D12Renderer::endShowFrame() {
    ZoneScopedN("endShowFrame");

    // Close the GPU zone before any early return so the destructor injects
    // the end-timestamp query into m_showCmdList while it is still open.
    // Single delete site covers all early-return paths below.
#if defined(ENTITY_ENABLE_TRACY) && defined(TRACY_ENABLE)
    delete tl_showGpuZone;
    tl_showGpuZone = nullptr;
#endif

    if (m_deviceLost) return;

    // Phase C.11: close + execute the COPY queue's command list before the
    // direct queue's. Always close (D3D12 requires it after Reset) but only
    // Signal/Wait when uploads were recorded.
    HRESULT chr = m_copyCommandList->Close();
    if (FAILED(chr)) {
        std::cerr << "Failed to close copy command list!" << std::endl;
        return;
    }
    if (m_uploadsRecordedThisFrame) {
        ID3D12CommandList* copyLists[] = { m_copyCommandList.Get() };
        m_gpu->copyQueue()->ExecuteCommandLists(1, copyLists);
        const uint64_t copyValue = ++m_uploadFenceValue;
        m_gpu->copyQueue()->Signal(m_uploadFence.Get(), copyValue);
        m_gpu->commandQueue()->Wait(m_uploadFence.Get(), copyValue);
    }

    // Close and execute show command list
    HRESULT hr = m_showCmdList->Close();
    if (FAILED(hr)) {
        std::cerr << "Failed to close show command list!" << std::endl;
        return;
    }
    ID3D12CommandList* showLists[] = { m_showCmdList.Get() };
    m_gpu->commandQueue()->ExecuteCommandLists(1, showLists);

    // Present all active output swap chains. The ExecuteCommandLists above
    // has committed all their work.
    for (auto& ow : m_outputWindows) {
        if (!ow.active || !ow.swapChain) continue;
        HRESULT ohr = ow.swapChain->Present(1, 0);
        if (FAILED(ohr)) {
            if (ohr == DXGI_ERROR_DEVICE_REMOVED || ohr == DXGI_ERROR_DEVICE_RESET ||
                ohr == DXGI_ERROR_DEVICE_HUNG) {
                handleDeviceLost(ohr, "Output Present");
                return;
            }
            std::cerr << "[OutputWindow] Present failed! HRESULT: 0x"
                      << std::hex << ohr << std::dec << std::endl;
        }
        ow.currentBackBufferIndex = ow.swapChain->GetCurrentBackBufferIndex();
    }

    // Signal show fence for this slot. m_showFenceValues[slot] holds the
    // next value to signal; after signaling we advance to slot+1 for the
    // next time this slot comes around. beginShowFrame gates on
    // showFenceValues[slot]-1 (the value just signaled) not the pending value.
    const uint64_t showValue = m_showFenceValues[m_showBackBufferIndex];
    m_gpu->commandQueue()->Signal(m_showFence.Get(), showValue);
    m_showFenceValues[m_showBackBufferIndex] = showValue + 1;

    // Collect Tracy GPU timestamp results for this frame.
    TracyD3D12Collect(m_tracyD3D12Ctx);

    // Stage 3d: after Signal, GPU work for this tick is committed. Publish
    // each compose target's write index as the new stable read index so the
    // editor thread can sample the freshly-rendered sub-resource via ImGui.
    for (auto& ct : m_composeTargets) {
        if (ct.ready) {
            ct.lastStableIndex.store(ct.writeIndex % ComposeTarget::TRIPLE,
                                     std::memory_order_release);
            // Advance to next sub-resource for the next show tick.
            ++ct.writeIndex;
        }
        // #75 liveness backstop: a closed gate must be re-asserted by a
        // resizeComposeTarget call every tick, or it was abandoned — the
        // requested dims reverted mid-deferral, the screen was deleted, or
        // the snapshot carried invalid dims that short-circuit before the
        // gate logic. Nothing else ever reopens a gate, so without this a
        // stranded gate blanks the editor preview for the slot permanently.
        // (open() is a no-op on an already-open gate.)
        if (!ct.resizeAttemptedThisTick) {
            ct.editorGate.open();
        }
        ct.resizeAttemptedThisTick = false;
    }

    tl_activeCmdList = nullptr;
}

void D3D12Renderer::beginEditorFrame() {
    ZoneScopedN("beginEditorFrame");
    // #75: count the frame unconditionally (before any early return) so it
    // stays paired with endEditorFrame's endFrame() calls, which also fire
    // on every exit path. A skewed pair starves compose-target resizes.
    m_editorFrameTracker.beginFrame();
    if (m_deviceLost) return;

    // m_currentBackBufferIndex selects which RTV/back-buffer to draw into
    // (used by clear() and endEditorFrame()'s present-state barrier) — keep it.
    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Ring slot drives allocator + fence reuse. Monotonic key matches ImGui's
    // FrameRenderBuffers ring (FrameIndex % numFramesInFlight) so the slot we
    // fence below in moveToNextFrame is exactly the slot ImGui reuses. Keying
    // on the back-buffer index instead desynced from ImGui under no-flip
    // presents and freed a live ImGui buffer (docs/perf/device-hung-2026-06).
    ++m_editorFrameCounter;
    m_editorRingSlot = static_cast<uint32_t>(m_editorFrameCounter % FRAME_COUNT);

    // Reap deferred mesh slot frees whose fence value the GPU has passed.
    if (m_meshUploader) {
        m_meshUploader->onFrameBegin(m_editorFence->GetCompletedValue());
    }

    // Reset editor-side allocator + command list
    m_editorAllocators[m_editorRingSlot]->Reset();
    m_editorCmdList->Reset(m_editorAllocators[m_editorRingSlot].Get(), nullptr);

    // Reset descriptor heap cache for editor recording
    tl_currentDescriptorHeap = nullptr;

    // Route draw calls to the editor command list
    tl_activeCmdList = m_editorCmdList.Get();

    // Set viewport and scissor to back-buffer size for ImGui
    D3D12_VIEWPORT viewport = {};
    viewport.Width    = static_cast<float>(m_width);
    viewport.Height   = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = {};
    scissorRect.right  = static_cast<LONG>(m_width);
    scissorRect.bottom = static_cast<LONG>(m_height);

    tl_activeCmdList->RSSetViewports(1, &viewport);
    tl_activeCmdList->RSSetScissorRects(1, &scissorRect);
}

void D3D12Renderer::endEditorFrame() {
    ZoneScopedN("endEditorFrame");
    // #75: pair beginEditorFrame's beginFrame() on EVERY exit path — a
    // missed endFrame() permanently defers compose-target resizes. The only
    // ordering requirement is "not before ExecuteCommandLists", and scope
    // exit always satisfies it; on the early-return paths nothing was (or
    // ever will be) submitted for this frame, so ending it is correct too.
    struct FrameEndGuard {
        EditorFrameTracker& tracker;
        ~FrameEndGuard() { tracker.endFrame(); }
    } frameEndGuard{m_editorFrameTracker};

    if (m_deviceLost) return;

    // Transition back buffer to PRESENT state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_currentBackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tl_activeCmdList->ResourceBarrier(1, &barrier);

    // Close and execute editor command list
    HRESULT hr = m_editorCmdList->Close();
    if (FAILED(hr)) {
        std::cerr << "Failed to close editor command list!" << std::endl;
        return;
    }
    ID3D12CommandList* editorLists[] = { m_editorCmdList.Get() };
    m_gpu->commandQueue()->ExecuteCommandLists(1, editorLists);

    // Present editor swap chain
    hr = m_swapChain->Present(1, 0);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
            hr == DXGI_ERROR_DEVICE_HUNG) {
            handleDeviceLost(hr, "Present");
            return;
        }
        std::cerr << "Failed to present swap chain! HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
    }

    tl_activeCmdList = nullptr;
    moveToNextFrame();
}

void D3D12Renderer::clear(float r, float g, float b, float a) {
    if (m_deviceLost.load(std::memory_order_relaxed)) return;

    // Transition render target to RENDER_TARGET state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_renderTargets[m_currentBackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    tl_activeCmdList->ResourceBarrier(1, &barrier);

    // Get RTV handle for current back buffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_currentBackBufferIndex * m_rtvDescriptorSize;

    // Set render target
    tl_activeCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear the render target
    const float clearColor[] = { r, g, b, a };
    tl_activeCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
}

// D3D12_AUTO_BREADCRUMB_OP -> name. Mapping verified against MS Learn; index
// is the enum value. Out-of-range ops print their raw integer (see caller).
static const char* dredOpName(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* const NAMES[] = {
        "SETMARKER", "BEGINEVENT", "ENDEVENT", "DRAWINSTANCED",
        "DRAWINDEXEDINSTANCED", "EXECUTEINDIRECT", "DISPATCH",
        "COPYBUFFERREGION", "COPYTEXTUREREGION", "COPYRESOURCE", "COPYTILES",
        "RESOLVESUBRESOURCE", "CLEARRENDERTARGETVIEW",
        "CLEARUNORDEREDACCESSVIEW", "CLEARDEPTHSTENCILVIEW", "RESOURCEBARRIER",
        "EXECUTEBUNDLE", "PRESENT", "RESOLVEQUERYDATA", "BEGINSUBMISSION",
        "ENDSUBMISSION", "DECODEFRAME", "PROCESSFRAMES", "ATOMICCOPYBUFFERUINT",
        "ATOMICCOPYBUFFERUINT64", "RESOLVESUBRESOURCEREGION",
        "WRITEBUFFERIMMEDIATE", "DECODEFRAME1", "SETPROTECTEDRESOURCESESSION",
        "DECODEFRAME2", "PROCESSFRAMES1",
        "BUILDRAYTRACINGACCELERATIONSTRUCTURE",
        "EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO",
        "COPYRAYTRACINGACCELERATIONSTRUCTURE", "DISPATCHRAYS",
        "INITIALIZEMETACOMMAND", "EXECUTEMETACOMMAND", "ESTIMATEMOTION",
        "RESOLVEMOTIONVECTORHEAP", "SETPIPELINESTATE1",
        "INITIALIZEEXTENSIONCOMMAND", "EXECUTEEXTENSIONCOMMAND", "DISPATCHMESH",
        "ENCODEFRAME", "RESOLVEENCODEROUTPUTMETADATA", "BARRIER",
        "BEGIN_COMMAND_LIST", "DISPATCHGRAPH", "SETPROGRAM", "PROCESSFRAMES2",
    };
    const UINT idx = static_cast<UINT>(op);
    return (idx < (sizeof(NAMES) / sizeof(NAMES[0]))) ? NAMES[idx] : nullptr;
}

// D3D12_DRED_ALLOCATION_TYPE -> name for the values we expect on a page fault.
// Anything not tabled prints its raw integer (see caller). RESOURCE (19) is the
// one we most expect to overlap a PageFaultVA.
static const char* dredAllocTypeName(D3D12_DRED_ALLOCATION_TYPE type) {
    switch (type) {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:   return "COMMAND_QUEUE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "COMMAND_ALLOCATOR";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:  return "PIPELINE_STATE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:    return "COMMAND_LIST";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE:           return "FENCE";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DESCRIPTOR_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP:            return "HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:      return "QUERY_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "COMMAND_SIGNATURE";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PIPELINE_LIBRARY";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER:   return "VIDEO_DECODER";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR: return "VIDEO_PROCESSOR";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:        return "RESOURCE";
        case D3D12_DRED_ALLOCATION_TYPE_PASS:            return "PASS";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION:   return "CRYPTOSESSION";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "CRYPTOSESSIONPOLICY";
        case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "PROTECTEDRESOURCESESSION";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP: return "VIDEO_DECODER_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL:    return "COMMAND_POOL";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "COMMAND_RECORDER";
        case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT:    return "STATE_OBJECT";
        case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND:     return "METACOMMAND";
        case D3D12_DRED_ALLOCATION_TYPE_SCHEDULINGGROUP: return "SCHEDULINGGROUP";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_ESTIMATOR: return "VIDEO_MOTION_ESTIMATOR";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_VECTOR_HEAP: return "VIDEO_MOTION_VECTOR_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_EXTENSION_COMMAND: return "VIDEO_EXTENSION_COMMAND";
        default: return nullptr;
    }
}

// Append a DRED_ALLOCATION_NODE1 linked list (existing or recently-freed) to
// the blob. Caps node count so the blob stays bounded. tag is "Existing" /
// "RecentFreed".
static void appendDredAllocationNodes(std::ostringstream& oss, const char* tag,
                                      const D3D12_DRED_ALLOCATION_NODE1* node) {
    constexpr int MAX_ALLOC_NODES = 16;
    int count = 0;
    while (node && count < MAX_ALLOC_NODES) {
        oss << tag << "=";
        if (node->ObjectNameA) {
            oss << node->ObjectNameA;
        } else if (node->ObjectNameW) {
            char nameBuf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, node->ObjectNameW, -1,
                                nameBuf, sizeof(nameBuf), nullptr, nullptr);
            oss << nameBuf;
        } else {
            oss << "<unnamed>";
        }
        oss << " type=";
        if (const char* tn = dredAllocTypeName(node->AllocationType)) {
            oss << tn;
        } else {
            oss << static_cast<int>(node->AllocationType);
        }
        oss << "\n";
        node = node->pNext;
        ++count;
    }
    if (node) {
        oss << tag << "=... (truncated at " << MAX_ALLOC_NODES << " nodes)\n";
    }
}

// Format DRED auto-breadcrumb nodes into a human-readable string.
// Uses std::string (heap intact at this call site — show thread controlled exit).
static std::string formatDredBreadcrumbs(ID3D12Device* device) {
    if (!device) return {};

    ComPtr<ID3D12DeviceRemovedExtendedData1> dredData;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dredData)))) return {};

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    D3D12_DRED_PAGE_FAULT_OUTPUT1       pageFault{};
    dredData->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    dredData->GetPageFaultAllocationOutput1(&pageFault);

    std::ostringstream oss;

    // Walk the breadcrumb linked list.
    const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
    int nodeIdx = 0;
    while (node) {
        oss << "[Node " << nodeIdx++ << "] ";

        // Prefer narrow (UTF-8) name; fall back to wide conversion.
        if (node->pCommandListDebugNameA) {
            oss << "CmdList=" << node->pCommandListDebugNameA;
        } else if (node->pCommandListDebugNameW) {
            char nameBuf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, node->pCommandListDebugNameW, -1,
                                nameBuf, sizeof(nameBuf), nullptr, nullptr);
            oss << "CmdList=" << nameBuf;
        }

        if (node->pCommandQueueDebugNameA) {
            oss << " Queue=" << node->pCommandQueueDebugNameA;
        } else if (node->pCommandQueueDebugNameW) {
            char nameBuf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, node->pCommandQueueDebugNameW, -1,
                                nameBuf, sizeof(nameBuf), nullptr, nullptr);
            oss << " Queue=" << nameBuf;
        }

        UINT lastOp = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        oss << " LastOp=" << lastOp << "/" << node->BreadcrumbCount;

        // If the list didn't finish (lastOp < total), decode a window of ops
        // around the last confirmed op so we can see what was executing when
        // the GPU hung. lastOp is the COUNT of completed ops; the history index
        // of the last confirmed op is (lastOp - 1) % 65536 (ring wraps past
        // 65536 ops — modulo is free, so apply it unconditionally).
        if (node->pCommandHistory && node->pLastBreadcrumbValue &&
            lastOp < node->BreadcrumbCount && node->BreadcrumbCount > 0) {
            const UINT lastConfirmed = (lastOp == 0) ? 0u : ((lastOp - 1u) % 65536u);
            const UINT begin = (lastConfirmed > 3u) ? (lastConfirmed - 3u) : 0u;
            const UINT end = ((lastConfirmed + 3u) < (node->BreadcrumbCount - 1u))
                                 ? (lastConfirmed + 3u)
                                 : (node->BreadcrumbCount - 1u);
            oss << "\n  ops:";
            for (UINT i = begin; i <= end; ++i) {
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                oss << " op[" << i << "]=";
                if (const char* opName = dredOpName(op)) {
                    oss << opName;
                } else {
                    oss << static_cast<UINT>(op);
                }
                if (lastOp > 0 && i == lastConfirmed) {
                    oss << "<--last confirmed";
                }
            }
        }

        // Breadcrumb contexts (UTF-16 strings — convert to UTF-8).
        for (UINT ci = 0; ci < node->BreadcrumbContextsCount; ++ci) {
            const auto& ctx = node->pBreadcrumbContexts[ci];
            if (ctx.pContextString) {
                char ctxBuf[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, ctx.pContextString, -1,
                                    ctxBuf, sizeof(ctxBuf), nullptr, nullptr);
                oss << " ctx[" << ctx.BreadcrumbIndex << "]=" << ctxBuf;
            }
        }
        oss << "\n";
        node = node->pNext;
    }

    // Page-fault virtual address + the allocations that owned (or recently
    // owned) it. RecentFreed overlap with no Existing node = use-after-free;
    // both lists empty with VA != 0 = never-valid address (bad offset math).
    if (pageFault.PageFaultVA != 0) {
        oss << "PageFaultVA=0x" << std::hex << pageFault.PageFaultVA << std::dec << "\n";
        appendDredAllocationNodes(oss, "Existing",
                                  pageFault.pHeadExistingAllocationNode);
        appendDredAllocationNodes(oss, "RecentFreed",
                                  pageFault.pHeadRecentFreedAllocationNode);
        if (!pageFault.pHeadExistingAllocationNode &&
            !pageFault.pHeadRecentFreedAllocationNode) {
            oss << "(no allocation nodes — VA was never valid; suspect bad offset math)\n";
        }
    }

    return oss.str();
}

HRESULT D3D12Renderer::removedReasonAfterTimeout() {
    if (!m_gpu || !m_gpu->isInitialized()) {
        return DXGI_ERROR_DEVICE_HUNG;
    }
    HRESULT reason = m_gpu->device()->GetDeviceRemovedReason();
    if (reason == S_OK) {
        // The fence timed out before the driver declared removal. Give the
        // driver a short grace period to finish raising the TDR, then re-query
        // once — otherwise a genuine hang is reported as a misleading 0x0.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        reason = m_gpu->device()->GetDeviceRemovedReason();
    }
    return reason;
}

void D3D12Renderer::handleDeviceLost(HRESULT hr, const char* site) {
    HRESULT removedReason = (m_gpu && m_gpu->isInitialized())
        ? m_gpu->device()->GetDeviceRemovedReason()
        : hr;
    // Publish the reason (release) BEFORE latching m_deviceLost below, so a
    // third-thread reader that observes the latch true (acquire) can never see
    // a stale S_OK reason — closing the window that would make the shutdown
    // gate and the device-lost report misread a confirmed removal as spurious.
    // Multi-writer note: when several threads hit device-lost concurrently they
    // all store here before the CAS; the writes can interleave, but every value
    // is a genuine GetDeviceRemovedReason() observation (last-writer-wins), and
    // every consumer fails safe on S_OK (the shutdown gate still drains, the
    // report shows 0x0-spurious). Do NOT "simplify" this to a single store
    // after the CAS — that reopens the latch-true-but-reason-S_OK race.
    m_deviceLostReason.store(removedReason, std::memory_order_release);

    // Single-CAS gate on the dbghelp crash path. Two threads can hit
    // device-lost concurrently (e.g. the editor Present site and a show-thread
    // fence timeout); dbghelp's MiniDumpWriteDump (reached via
    // CrashLogger::captureNonFatal below) is process-global and NOT
    // thread-safe, so a relaxed load-then-store latch let both threads enter
    // and deadlock inside dbghelp (the paired 0-byte-then-complete crash dirs).
    // compare_exchange_strong ensures exactly one thread owns the crash path.
    bool expected = false;
    if (!m_deviceLost.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        return;  // another thread already owns the crash path
    }

    std::cerr << "=======================================================" << std::endl;
    std::cerr << "[D3D12] GPU DEVICE LOST at " << site << std::endl;
    std::cerr << "        HRESULT:         0x" << std::hex << hr << std::dec << std::endl;
    std::cerr << "        RemovedReason:   0x" << std::hex << removedReason << std::dec << std::endl;
    switch (removedReason) {
        case DXGI_ERROR_DEVICE_HUNG:
            std::cerr << "        = DXGI_ERROR_DEVICE_HUNG (GPU took too long to respond — driver timeout)" << std::endl;
            break;
        case DXGI_ERROR_DEVICE_REMOVED:
            std::cerr << "        = DXGI_ERROR_DEVICE_REMOVED (GPU was physically removed or driver crashed)" << std::endl;
            break;
        case DXGI_ERROR_DEVICE_RESET:
            std::cerr << "        = DXGI_ERROR_DEVICE_RESET (GPU had a hardware error not caused by this app)" << std::endl;
            break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            std::cerr << "        = DXGI_ERROR_DRIVER_INTERNAL_ERROR (driver bug)" << std::endl;
            break;
        default:
            std::cerr << "        = (unknown reason)" << std::endl;
            break;
    }
    std::cerr << "        Engine will shut down. Restart the application." << std::endl;
    std::cerr << "=======================================================" << std::endl;

    // Capture DRED breadcrumbs and write forensic crash log.
    std::string dredBlob = (m_gpu && m_gpu->isInitialized())
        ? formatDredBreadcrumbs(m_gpu->device())
        : std::string{};
    CrashLogger::captureNonFatal("device-lost",
                                  static_cast<long>(removedReason),
                                  dredBlob,
                                  site);
}

ID3D12Device* D3D12Renderer::getDevice() const {
    return m_gpu ? m_gpu->device() : nullptr;
}

ID3D12CommandQueue* D3D12Renderer::getCommandQueue() const {
    return m_gpu ? m_gpu->commandQueue() : nullptr;
}

ID3D12Fence* D3D12Renderer::getShowFence() const {
    return m_showFence.Get();
}

Result D3D12Renderer::createSwapChain(void* windowHandle, uint32_t width, uint32_t height) {
    // Cache the DXGI factory on the renderer so per-output swap chains can reuse it.
    if (!m_dxgiFactory) {
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_dxgiFactory));
        if (FAILED(hr)) {
            std::cerr << "Failed to create DXGI factory!" << std::endl;
            return Result::Failure;
        }
    }

    // Describe swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = 0;

    // Create swap chain
    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = m_dxgiFactory->CreateSwapChainForHwnd(
        m_gpu->commandQueue(),
        static_cast<HWND>(windowHandle),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create swap chain!" << std::endl;
        return Result::Failure;
    }

    // Cast to IDXGISwapChain3
    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        std::cerr << "Failed to cast swap chain to IDXGISwapChain3!" << std::endl;
        return Result::Failure;
    }

    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    std::cout << "Swap chain created (" << width << "x" << height << ")" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createRenderTargetViews() {
    // Create RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = m_gpu->device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create RTV descriptor heap!" << std::endl;
        return Result::Failure;
    }

    m_rtvDescriptorSize = m_gpu->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create RTVs for each frame
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            std::cerr << "Failed to get swap chain buffer " << i << "!" << std::endl;
            return Result::Failure;
        }
        {
            wchar_t name[32];
            swprintf_s(name, L"SwapChainBuf%u", i);
            m_renderTargets[i]->SetName(name);
        }

        m_gpu->device()->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    std::cout << "Render target views created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createCommandAllocators() {
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        HRESULT hr = m_gpu->device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_editorAllocators[i])
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create editor command allocator " << i << "!" << std::endl;
            return Result::Failure;
        }

        hr = m_gpu->device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_showAllocators[i])
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create show command allocator " << i << "!" << std::endl;
            return Result::Failure;
        }
    }

    std::cout << "Command allocators created (editor + show)" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createCommandList() {
    HRESULT hr = m_gpu->device()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_editorAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_editorCmdList)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create editor command list!" << std::endl;
        return Result::Failure;
    }
    m_editorCmdList->SetName(L"EditorCmdList");
    m_editorCmdList->Close();

    hr = m_gpu->device()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_showAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_showCmdList)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create show command list!" << std::endl;
        return Result::Failure;
    }
    m_showCmdList->SetName(L"ShowCmdList");
    m_showCmdList->Close();

    std::cout << "Command lists created (editor + show)" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createCopyCommandList() {
    // Per-frame COPY allocators so the previous frame's recorded copies aren't
    // reset out from under the GPU. moveToNextFrame's fence on the direct queue
    // implicitly fences copy work too (direct waits on copy before signaling),
    // so the same backbuffer-index ring is safe.
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        HRESULT hr = m_gpu->device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY,
            IID_PPV_ARGS(&m_copyCommandAllocators[i])
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create copy command allocator " << i << "!" << std::endl;
            return Result::Failure;
        }
    }

    HRESULT hr = m_gpu->device()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COPY,
        m_copyCommandAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_copyCommandList)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create copy command list!" << std::endl;
        return Result::Failure;
    }
    m_copyCommandList->SetName(L"ShowCopyCmdList");
    m_copyCommandList->Close();  // Lists start in recording state; close so beginShowFrame's Reset works.

    // Editor-thread copy allocators + list for uploadMeshImmediate (separate
    // from the show-thread copy resources to avoid concurrent recording).
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        hr = m_gpu->device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY,
            IID_PPV_ARGS(&m_editorCopyCommandAllocators[i])
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create editor copy command allocator " << i << "!" << std::endl;
            return Result::Failure;
        }
    }
    hr = m_gpu->device()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COPY,
        m_editorCopyCommandAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_editorCopyCommandList)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create editor copy command list!" << std::endl;
        return Result::Failure;
    }
    m_editorCopyCommandList->SetName(L"EditorCopyCmdList");
    m_editorCopyCommandList->Close();

    std::cout << "Copy command lists created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createFence() {
    HRESULT hr = m_gpu->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_editorFence));
    if (FAILED(hr)) {
        std::cerr << "Failed to create editor fence!" << std::endl;
        return Result::Failure;
    }

    hr = m_gpu->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_showFence));
    if (FAILED(hr)) {
        std::cerr << "Failed to create show fence!" << std::endl;
        return Result::Failure;
    }

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_editorFenceValues[i] = 1;
        m_showFenceValues[i]   = 1;
    }

    // Phase C.11: separate fence for cross-queue ordering (copy → direct).
    // Monotonic value, not per-frame; copy queue Signals after each frame's
    // upload Execute, direct queue Wait()s before its own Execute.
    hr = m_gpu->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_uploadFence));
    if (FAILED(hr)) {
        std::cerr << "Failed to create upload fence!" << std::endl;
        return Result::Failure;
    }
    m_uploadFenceValue = 0;

    // Per-thread fence events: editor thread uses m_fenceEvent;
    // show thread uses m_showFenceEvent. Never share — concurrent
    // SetEventOnCompletion on the same handle is a data race.
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        std::cerr << "Failed to create fence event!" << std::endl;
        return Result::Failure;
    }
    m_showFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_showFenceEvent) {
        std::cerr << "Failed to create show fence event!" << std::endl;
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
        return Result::Failure;
    }

    std::cout << "Fence created" << std::endl;
    return Result::Success;
}

void D3D12Renderer::waitForGpu() {
    ZoneScopedNC("GPU fence wait", 0xCC4444);
    // Top-level early-out: if the device is already lost, no fence will ever
    // signal again. Return immediately without touching any D3D12 COM objects.
    // Also guards the nested per-branch checks below against the race where
    // device-lost transitions while we're mid-drain.
    if (m_deviceLost) return;

    // Use a function-local event so this call cannot race with concurrent
    // SetEventOnCompletion/WaitForSingleObject pairs on m_fenceEvent. Auto-reset
    // events are FIFO-ish but only wake one waiter per signal; the editor
    // thread's moveToNextFrame waits on m_fenceEvent with a 2-second timeout,
    // and the show thread enters waitForGpu via resizeComposeTarget. With both
    // arming m_fenceEvent on the same fence, whichever Wait runs first steals
    // the wakeup and the other times out — surfaces as a false device-lost.
    HANDLE localEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!localEvent) {
        std::cerr << "[D3D12] waitForGpu: CreateEvent failed" << std::endl;
        return;
    }

    // Drain the COPY queue first (Phase C.11). Resize/shutdown/freeSlot must
    // not release a texture while an upload is still in flight on the copy
    // queue. The direct queue's Wait(uploadFence) inside endShowFrame() does NOT
    // help here because we may be called outside the per-frame loop.
    if (m_uploadFence && m_gpu && m_gpu->copyQueue()) {
        if (m_deviceLost) { CloseHandle(localEvent); return; }
        const uint64_t copyTarget = ++m_uploadFenceValue;
        m_gpu->copyQueue()->Signal(m_uploadFence.Get(), copyTarget);
        if (m_uploadFence->GetCompletedValue() < copyTarget) {
            m_uploadFence->SetEventOnCompletion(copyTarget, localEvent);
            DWORD result = WaitForSingleObject(localEvent, fenceWaitTimeoutMs());
            if (result == WAIT_TIMEOUT || result == WAIT_FAILED) {
                HRESULT removedReason = removedReasonAfterTimeout();
                handleDeviceLost(removedReason, "waitForGpu copy-queue drain timeout");
                CloseHandle(localEvent);
                return;
            }
        }
    }

    if (!m_gpu) { CloseHandle(localEvent); return; }

    // Drain both show and editor fences so all direct-queue work is complete.
    // Use a fresh monotonic value (GetCompletedValue + 1) so the Signal is
    // always strictly greater than the last signaled value — re-signaling the
    // same value is E_INVALIDARG in D3D12. Per-slot fence values
    // (m_showFenceValues / m_editorFenceValues) are NOT updated here; they are
    // owned by endShowFrame / moveToNextFrame and must be left intact so the
    // per-frame allocator-reuse gate still works correctly after this call.
    if (m_showFence) {
        if (m_deviceLost) { CloseHandle(localEvent); return; }
        const uint64_t drainValue = m_showFence->GetCompletedValue() + 1;
        m_gpu->commandQueue()->Signal(m_showFence.Get(), drainValue);
        m_showFence->SetEventOnCompletion(drainValue, localEvent);
        DWORD result = WaitForSingleObject(localEvent, fenceWaitTimeoutMs());
        if (result == WAIT_TIMEOUT || result == WAIT_FAILED) {
            HRESULT removedReason = removedReasonAfterTimeout();
            handleDeviceLost(removedReason, "waitForGpu show-fence drain timeout");
            CloseHandle(localEvent);
            return;
        }
    }

    if (m_editorFence) {
        if (m_deviceLost) { CloseHandle(localEvent); return; }
        const uint64_t drainValue = m_editorFence->GetCompletedValue() + 1;
        m_gpu->commandQueue()->Signal(m_editorFence.Get(), drainValue);
        m_editorFence->SetEventOnCompletion(drainValue, localEvent);
        DWORD result = WaitForSingleObject(localEvent, fenceWaitTimeoutMs());
        if (result == WAIT_TIMEOUT || result == WAIT_FAILED) {
            HRESULT removedReason = removedReasonAfterTimeout();
            handleDeviceLost(removedReason, "waitForGpu editor-fence drain timeout");
            CloseHandle(localEvent);
            return;
        }
    }

    CloseHandle(localEvent);
}

void D3D12Renderer::moveToNextFrame() {
    ZoneScopedN("moveToNextFrame");
    // Frame-pacing keyed on the MONOTONIC ring slot (m_editorRingSlot), not the
    // swap-chain back-buffer index. m_editorFenceValues[slot] is the high-water
    // "next value to signal" for that slot. We:
    //   1. Signal currentFenceValue for the slot we just recorded,
    //   2. Wait until the GPU has finished the slot the NEXT frame will reuse,
    //   3. Then record nextSlot's high-water value (currentFenceValue + 1) for
    //      that next frame's wait gate.
    //
    // The wait target is computed from the counter ((counter+1) % FRAME_COUNT),
    // not from GetCurrentBackBufferIndex(). That is the whole point of the fix:
    // ImGui's backend reuses (and grows-and-frees) FrameRenderBuffers keyed on
    // its own monotonic counter, so the slot we must fence before returning is
    // the counter-derived slot, regardless of what the swap chain reports under
    // a no-flip present. See docs/perf/device-hung-2026-06/diagnosis.md.
    //
    // The std::max keeps signalValue strictly greater than the fence's current
    // completed value: waitForGpu() can drain editorFence beyond
    // editorValues[slot] mid-session, and Signal must be monotonic.
    const uint64_t currentFenceValue = std::max(m_editorFenceValues[m_editorRingSlot],
                                                m_editorFence->GetCompletedValue() + 1);
    m_gpu->commandQueue()->Signal(m_editorFence.Get(), currentFenceValue);

    // The next frame will use slot ((counter+1) % FRAME_COUNT). Wait until the
    // GPU has finished that slot's PREVIOUS submission before we return, so
    // beginEditorFrame's allocator reset AND ImGui's grow-and-free of that same
    // slot are both safe. This is the slot-reuse fence the ImGui backend relies
    // on the caller to provide.
    //
    // Use a finite timeout (MED-13) so a GPU hang surfaces as device-lost
    // rather than a silent process freeze. On timeout, GetDeviceRemovedReason()
    // returns a meaningful HRESULT if the driver crashed (TDR, etc.).
    const uint32_t nextSlot =
        static_cast<uint32_t>((m_editorFrameCounter + 1) % FRAME_COUNT);
    if (m_editorFence->GetCompletedValue() < m_editorFenceValues[nextSlot]) {
        ZoneScopedNC("GPU fence wait", 0xCC4444);
        m_editorFence->SetEventOnCompletion(m_editorFenceValues[nextSlot], m_fenceEvent);
        DWORD result = WaitForSingleObject(m_fenceEvent, fenceWaitTimeoutMs());
        if (result == WAIT_TIMEOUT || result == WAIT_FAILED) {
            HRESULT removedReason = removedReasonAfterTimeout();
            handleDeviceLost(removedReason, "moveToNextFrame fence wait timeout");
            return;
        }
    }

    // Record the high-water fence value for nextSlot (the slot the next frame
    // will reuse). currentFenceValue + 1 is deliberately one beyond what was
    // ever signaled, so next frame's wait blocks only if the GPU is genuinely
    // behind. Mirrors the original swap-chain-keyed pacing exactly — the only
    // change is the key (monotonic counter, not back-buffer index), so the
    // fenced slot now always equals ImGui's reuse slot.
    m_editorFenceValues[nextSlot] = currentFenceValue + 1;
}

// ============================================================================
// Rendering Pipeline Methods
// ============================================================================

Result D3D12Renderer::loadCompiledShader(const std::wstring& csoFilename, ID3DBlob** blob) {
    // Read a DXC-precompiled DXIL blob from disk. Shaders are built by
    // shaders/CMakeLists.txt and copied to <exe_dir>/shaders/<name>.cso.
    // We anchor to the executable directory so integration tests that launch
    // the app from the project root still resolve paths correctly.
    wchar_t exePath[MAX_PATH];
    DWORD exePathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path fullPath;
    if (exePathLen > 0 && exePathLen < MAX_PATH) {
        fullPath = std::filesystem::path(exePath).parent_path() / csoFilename;
    } else {
        // Fall back to cwd-relative if we somehow can't determine the exe path
        fullPath = csoFilename;
    }

    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::wcerr << L"Compiled shader not found: " << fullPath.wstring()
                   << L" (did the shader build step run?)" << std::endl;
        return Result::Failure;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        std::wcerr << L"Compiled shader is empty: " << csoFilename << std::endl;
        return Result::Failure;
    }
    file.seekg(0, std::ios::beg);

    // Allocate a D3DBlob and slurp the file into it. D3DCreateBlob gives us
    // the ID3DBlob interface PSO creation expects without forcing a copy into
    // a second buffer.
    ComPtr<ID3DBlob> outBlob;
    HRESULT hr = D3DCreateBlob(static_cast<SIZE_T>(size), &outBlob);
    if (FAILED(hr)) {
        std::cerr << "D3DCreateBlob failed for shader load, HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return Result::Failure;
    }

    file.read(static_cast<char*>(outBlob->GetBufferPointer()), size);
    if (!file) {
        std::wcerr << L"Short read from compiled shader: " << csoFilename << std::endl;
        return Result::Failure;
    }

    *blob = outBlob.Detach();
    return Result::Success;
}

Result D3D12Renderer::createRootSignature() {
    // Define root parameter using 32-bit constants (not CBV!)
    // This ensures each draw call gets its own constant values, because
    // SetGraphicsRoot32BitConstants copies data into the command buffer
    // at record time, unlike CBV which just stores a pointer.
    //
    // LayerConstants = 28 dwords = 112 bytes (transform 16 + color 4 + opacity+blendMode+colorSpace+pad 4 + uvRect 4).
    // M3 (ADR-0021) added uvRect for Tiled source cropping.
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameter.Constants.ShaderRegister = 0; // b0
    rootParameter.Constants.RegisterSpace = 0;
    rootParameter.Constants.Num32BitValues = 28; // sizeof(LayerConstants) / 4
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Define root signature
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.NumStaticSamplers = 0;
    rootSignatureDesc.pStaticSamplers = nullptr;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize root signature
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cerr << "Root signature serialization failed: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return Result::Failure;
    }

    // Create root signature
    hr = m_gpu->device()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) {
        std::cerr << "Failed to create root signature!" << std::endl;
        return Result::Failure;
    }

    return Result::Success;
}

Result D3D12Renderer::createPipelineState() {
    // Compile shaders
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    if (loadCompiledShader(L"shaders/composite_vs.cso", &vertexShader) != Result::Success) {
        std::cerr << "Failed to load composite_vs.cso!" << std::endl;
        return Result::Failure;
    }

    if (loadCompiledShader(L"shaders/solid_color_ps.cso", &pixelShader) != Result::Success) {
        std::cerr << "Failed to load solid_color_ps.cso!" << std::endl;
        return Result::Failure;
    }

    // Define vertex input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Define blend state for alpha blending
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Define rasterizer state
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;

    // Define pipeline state
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    // Compose-target PSO — Phase C.12 #3 flips the working space to FP16
    // linear so OCIO ODT in mapping_surface_ps has unclamped headroom for
    // HAP HDR + ACEScg compositing. Swap chains stay UNORM8.
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr)) {
        std::cerr << "Failed to create pipeline state!" << std::endl;
        return Result::Failure;
    }

    return Result::Success;
}

Result D3D12Renderer::createVertexBuffer() {
    // Define a fullscreen quad with texture coordinates
    struct Vertex {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT2 texCoord;
    };

    Vertex vertices[] = {
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } }, // Bottom-left
        { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f } }, // Top-left
        { {  1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } }, // Bottom-right
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } }  // Top-right
    };

    const UINT vertexBufferSize = sizeof(vertices);

    // Create vertex buffer in upload heap
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = vertexBufferSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_vertexBuffer)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create vertex buffer!" << std::endl;
        return Result::Failure;
    }
    m_vertexBuffer->SetName(L"QuadVertexBuffer");

    // Copy vertex data to buffer
    void* vertexDataPtr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_vertexBuffer->Map(0, &readRange, &vertexDataPtr);
    if (FAILED(hr)) {
        std::cerr << "Failed to map vertex buffer!" << std::endl;
        return Result::Failure;
    }

    memcpy(vertexDataPtr, vertices, vertexBufferSize);
    m_vertexBuffer->Unmap(0, nullptr);

    // Initialize vertex buffer view
    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    m_vertexBufferView.SizeInBytes = vertexBufferSize;

    return Result::Success;
}

Result D3D12Renderer::createConstantBuffer() {
    // Create constant buffer in upload heap
    const UINT constantBufferSize = (sizeof(LayerConstants) + 255) & ~255; // Align to 256 bytes

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = constantBufferSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_constantBuffer)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create constant buffer!" << std::endl;
        return Result::Failure;
    }
    m_constantBuffer->SetName(L"LegacyConstantBuffer");

    // Map constant buffer (keep it mapped for updates)
    // NOTE: This mapped buffer is currently UNUSED - drawColoredQuad uses root constants instead.
    // If this buffer is ever used, it creates a CPU/GPU race condition (see CRIT-04 in code review).
    // Proper fix would be a ring buffer of constant buffers (one per frame in flight).
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_constantBuffer->Map(0, &readRange, &m_constantBufferData);
    if (FAILED(hr)) {
        std::cerr << "Failed to map constant buffer!" << std::endl;
        return Result::Failure;
    }

    return Result::Success;
}

void D3D12Renderer::updateConstantBuffer(const DirectX::XMMATRIX& transform, const DirectX::XMFLOAT4& color, float opacity) {
    // WARNING: This function is DEPRECATED and should not be used!
    // It writes to a persistently mapped upload heap buffer without GPU synchronization,
    // creating a race condition where the GPU may read stale data from previous frames.
    // drawColoredQuad correctly uses root constants instead (SetGraphicsRoot32BitConstants).
    // TODO: Remove this function and m_constantBuffer entirely if no longer needed.
    LayerConstants constants;
    DirectX::XMStoreFloat4x4(&constants.transform, DirectX::XMMatrixTranspose(transform));
    constants.color = color;
    constants.opacity = opacity;
    constants.blendMode = 0;  // Normal blend mode for colored quads
    constants.colorSpace = 0; // Linear — colored quads carry no texture
    constants.padding3 = 0.0f;

    memcpy(m_constantBufferData, &constants, sizeof(LayerConstants));
}

void D3D12Renderer::drawColoredQuad(const DirectX::XMMATRIX& transform, const DirectX::XMFLOAT4& color, float opacity) {
    if (!m_initialized) {
        return;
    }

    // Build constants structure
    LayerConstants constants;
    DirectX::XMStoreFloat4x4(&constants.transform, DirectX::XMMatrixTranspose(transform));
    constants.color = color;
    constants.opacity = opacity;
    constants.blendMode = 0;  // Normal blend mode for colored quads
    constants.colorSpace = 0; // Linear — colored quads carry no texture
    constants.padding3 = 0.0f;

    // Set pipeline state
    tl_activeCmdList->SetPipelineState(m_pipelineState.Get());
    tl_activeCmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set root constants (copies data into command buffer - each draw gets its own values!)
    tl_activeCmdList->SetGraphicsRoot32BitConstants(0, 28, &constants, 0);

    // NOTE: Don't override viewport/scissor - caller (beginComposeTarget or beginShowFrame) sets these
    // This allows drawColoredQuad to work correctly with both main window and offscreen targets

    // Set vertex buffer and draw
    tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    tl_activeCmdList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    tl_activeCmdList->DrawInstanced(4, 1, 0, 0);
}

// ============================================================================
// ImGui Integration
// ============================================================================

Result D3D12Renderer::initializeImGui(GLFWwindow* window) {
    // Create descriptor heap for ImGui SRV (fonts/textures + video textures)
    // Descriptor heap layout is owned by DescriptorHeapLayout (see header).
    // Slots: [0]=ImGui, [1]=legacy video, [compose targets...], [video textures...]
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = DescriptorHeapLayout::TOTAL_SLOTS;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_gpu->device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_imguiSrvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create ImGui descriptor heap!" << std::endl;
        return Result::Failure;
    }

    // Store descriptor size for later use
    m_srvDescriptorSize = m_gpu->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keep ImGui keyboard nav OFF — the app drives arrow keys directly via
    // GLFW for timeline scrubbing/frame-step. With nav on, ImGui captures
    // arrows to navigate widgets (e.g. focuses a button in StageWindow's
    // toolbar and auto-scrolls the window to it, making the 3D view appear
    // "disappeared" or cropped). Docking stays on.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Use default docking behavior (ConfigDockingWithShift = false)
    // This allows docking preview to appear when dragging windows

    // Setup Dear ImGui style - Custom modern theme
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Rounding for a softer, modern look
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 6.0f;

        // Spacing and padding
        style.WindowPadding = ImVec2(10.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.IndentSpacing = 20.0f;
        style.ScrollbarSize = 14.0f;
        style.GrabMinSize = 12.0f;

        // Borders
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;

        // Colors - Soft modern palette with teal/cyan accents
        ImVec4* colors = style.Colors;

        // Backgrounds - soft dark grays with slight warmth
        colors[ImGuiCol_WindowBg]             = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        colors[ImGuiCol_ChildBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_PopupBg]              = ImVec4(0.14f, 0.14f, 0.16f, 0.98f);

        // Text
        colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
        colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.54f, 1.00f);

        // Borders - subtle
        colors[ImGuiCol_Border]               = ImVec4(0.28f, 0.28f, 0.32f, 0.60f);
        colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

        // Frame backgrounds (input fields, etc.)
        colors[ImGuiCol_FrameBg]              = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
        colors[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.28f, 0.34f, 1.00f);

        // Title bar - slightly lighter
        colors[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_TitleBgActive]        = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.10f, 0.12f, 0.75f);

        // Menu bar
        colors[ImGuiCol_MenuBarBg]            = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);

        // Scrollbar
        colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.32f, 0.32f, 0.36f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.44f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.48f, 0.48f, 0.52f, 1.00f);

        // Checkmark/radio - teal accent
        colors[ImGuiCol_CheckMark]            = ImVec4(0.40f, 0.80f, 0.75f, 1.00f);

        // Slider grab - teal accent
        colors[ImGuiCol_SliderGrab]           = ImVec4(0.40f, 0.75f, 0.70f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.50f, 0.85f, 0.80f, 1.00f);

        // Buttons - soft teal
        colors[ImGuiCol_Button]               = ImVec4(0.25f, 0.52f, 0.50f, 1.00f);
        colors[ImGuiCol_ButtonHovered]        = ImVec4(0.30f, 0.62f, 0.58f, 1.00f);
        colors[ImGuiCol_ButtonActive]         = ImVec4(0.35f, 0.72f, 0.68f, 1.00f);

        // Headers (collapsing headers, tree nodes, selectables)
        colors[ImGuiCol_Header]               = ImVec4(0.25f, 0.52f, 0.50f, 0.50f);
        colors[ImGuiCol_HeaderHovered]        = ImVec4(0.30f, 0.62f, 0.58f, 0.70f);
        colors[ImGuiCol_HeaderActive]         = ImVec4(0.35f, 0.72f, 0.68f, 0.85f);

        // Separator
        colors[ImGuiCol_Separator]            = ImVec4(0.28f, 0.28f, 0.32f, 0.50f);
        colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.40f, 0.75f, 0.70f, 0.70f);
        colors[ImGuiCol_SeparatorActive]      = ImVec4(0.40f, 0.75f, 0.70f, 1.00f);

        // Resize grip
        colors[ImGuiCol_ResizeGrip]           = ImVec4(0.40f, 0.75f, 0.70f, 0.25f);
        colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.40f, 0.75f, 0.70f, 0.65f);
        colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.40f, 0.75f, 0.70f, 0.90f);

        // Tabs - soft and inviting
        colors[ImGuiCol_Tab]                  = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
        colors[ImGuiCol_TabHovered]           = ImVec4(0.30f, 0.62f, 0.58f, 0.80f);
        colors[ImGuiCol_TabActive]            = ImVec4(0.22f, 0.48f, 0.45f, 1.00f);
        colors[ImGuiCol_TabUnfocused]         = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.18f, 0.38f, 0.36f, 1.00f);

        // Docking
        colors[ImGuiCol_DockingPreview]       = ImVec4(0.40f, 0.75f, 0.70f, 0.70f);
        colors[ImGuiCol_DockingEmptyBg]       = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);

        // Plot lines
        colors[ImGuiCol_PlotLines]            = ImVec4(0.60f, 0.85f, 0.80f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]     = ImVec4(0.70f, 0.95f, 0.90f, 1.00f);
        colors[ImGuiCol_PlotHistogram]        = ImVec4(0.40f, 0.75f, 0.70f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.50f, 0.85f, 0.80f, 1.00f);

        // Table
        colors[ImGuiCol_TableHeaderBg]        = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]    = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
        colors[ImGuiCol_TableBorderLight]     = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
        colors[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

        // Text selection
        colors[ImGuiCol_TextSelectedBg]       = ImVec4(0.40f, 0.75f, 0.70f, 0.35f);

        // Drag and drop
        colors[ImGuiCol_DragDropTarget]       = ImVec4(0.50f, 0.90f, 0.85f, 0.90f);

        // Nav highlight
        colors[ImGuiCol_NavHighlight]         = ImVec4(0.40f, 0.75f, 0.70f, 1.00f);
        colors[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

        // Modal dimming
        colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
    }

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplDX12_Init(
        m_gpu->device(),
        FRAME_COUNT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_imguiSrvHeap.Get(),
        m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart()
    );

    std::cout << "ImGui initialized successfully!" << std::endl;
    return Result::Success;
}

void D3D12Renderer::shutdownImGui() {
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_imguiSrvHeap.Reset();
}

void D3D12Renderer::beginImGuiFrame() {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void D3D12Renderer::endImGuiFrame() {
    if (m_deviceLost.load(std::memory_order_relaxed)) return;

    ImGui::Render();

    // Set ImGui descriptor heap (only if not already set)
    if (tl_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        tl_activeCmdList->SetDescriptorHeaps(1, heaps);
        tl_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    // Render ImGui draw data
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), tl_activeCmdList);
}

// ============================================================================
// Video Texture Upload
// ============================================================================

void* D3D12Renderer::uploadVideoFrame(const uint8_t* rgbaData, uint32_t width, uint32_t height) {
    if (!m_initialized || !rgbaData || width == 0 || height == 0) {
        return nullptr;
    }

    HRESULT hr;

    // Check if we need to recreate the texture (size changed)
    if (m_videoTexture && (m_videoTextureWidth != width || m_videoTextureHeight != height)) {
        // Wait for GPU before releasing resources
        waitForGpu();
        m_videoTexture.Reset();
        m_videoUploadBuffer.Reset();
        m_videoTextureWidth = 0;
        m_videoTextureHeight = 0;
        m_videoTextureFirstUpload = true;
    }

    // Create texture if it doesn't exist
    if (!m_videoTexture) {
        // Create the texture resource
        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        hr = m_gpu->device()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_videoTexture)
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create video texture resource!" << std::endl;
            return nullptr;
        }
        m_videoTexture->SetName(L"LegacyVideoTex");

        // Create upload buffer
        UINT64 uploadBufferSize = 0;
        m_gpu->device()->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadBufferDesc = {};
        uploadBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadBufferDesc.Width = uploadBufferSize;
        uploadBufferDesc.Height = 1;
        uploadBufferDesc.DepthOrArraySize = 1;
        uploadBufferDesc.MipLevels = 1;
        uploadBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadBufferDesc.SampleDesc.Count = 1;
        uploadBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = m_gpu->device()->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_videoUploadBuffer)
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create video upload buffer!" << std::endl;
            m_videoTexture.Reset();
            return nullptr;
        }
        m_videoUploadBuffer->SetName(L"LegacyVideoUpload");

        // Create SRV for the texture (at descriptor index 1, after ImGui font)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = DescriptorHeapLayout::cpuHandle(
            m_imguiSrvHeap.Get(), DescriptorHeapLayout::LEGACY_VIDEO_SLOT, m_srvDescriptorSize);

        m_gpu->device()->CreateShaderResourceView(m_videoTexture.Get(), &srvDesc, cpuHandle);

        m_videoTextureGpuHandle = DescriptorHeapLayout::gpuHandle(
            m_imguiSrvHeap.Get(), DescriptorHeapLayout::LEGACY_VIDEO_SLOT, m_srvDescriptorSize);

        m_videoTextureWidth = width;
        m_videoTextureHeight = height;

        std::cout << "Created video texture: " << width << "x" << height << std::endl;
    }

    // Upload texture data
    D3D12_RESOURCE_DESC textureDesc = m_videoTexture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSizeInBytes;
    UINT64 totalBytes;

    m_gpu->device()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    // Map upload buffer and copy data
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };  // We don't read from this buffer
    hr = m_videoUploadBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        std::cerr << "Failed to map video upload buffer!" << std::endl;
        return nullptr;
    }

    // Copy row by row (handling pitch differences)
    uint8_t* dstPtr = static_cast<uint8_t*>(mappedData) + footprint.Offset;
    const uint8_t* srcPtr = rgbaData;
    UINT srcRowPitch = width * 4;

    for (UINT row = 0; row < numRows; ++row) {
        memcpy(dstPtr + row * footprint.Footprint.RowPitch,
               srcPtr + row * srcRowPitch,
               srcRowPitch);
    }

    m_videoUploadBuffer->Unmap(0, nullptr);

    // Record copy command
    // First, transition texture to COPY_DEST state if needed
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_videoTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Only transition if texture was previously used
    if (!m_videoTextureFirstUpload) {
        tl_activeCmdList->ResourceBarrier(1, &barrier);
    }
    m_videoTextureFirstUpload = false;

    // Copy from upload buffer to texture
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = m_videoUploadBuffer.Get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = m_videoTexture.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    tl_activeCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    // Transition texture back to shader resource state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    tl_activeCmdList->ResourceBarrier(1, &barrier);

    return reinterpret_cast<void*>(m_videoTextureGpuHandle.ptr);
}

void* D3D12Renderer::getVideoTextureID() const {
    if (m_videoTextureGpuHandle.ptr != 0) {
        return reinterpret_cast<void*>(m_videoTextureGpuHandle.ptr);
    }
    return nullptr;
}

// ============================================================================
// Multi-Texture Support
// ============================================================================

uint32_t D3D12Renderer::allocateVideoTextureSlot() {
    if (!m_textureUploader) return UINT32_MAX;
    const uint32_t slot = m_textureUploader->allocateSlot();
    if (slot == TextureUploader::INVALID_SLOT) {
        std::cerr << "No available video texture slots!" << std::endl;
        return UINT32_MAX;
    }
    std::cout << "Allocated video texture slot " << slot << std::endl;
    return slot;
}

bool D3D12Renderer::prepareVideoTextureSlot(uint32_t slot,
                                             uint32_t width, uint32_t height,
                                             TextureFormat format) {
    // 2026-05-23 — proactive D3D12 texture allocation. Called from
    // ProjectManager::loadMedia (editor thread) right after
    // allocateVideoTextureSlot so the show-thread first-frame upload
    // doesn't pay CreateCommittedResource on the hot path. See ADR-0014's
    // editor/show split — this is editor-thread D3D12 work, which is safe
    // for CreateCommittedResource / CreateShaderResourceView because the
    // slot isn't yet visible to the show thread (the SceneSnapshot bake
    // hasn't published this clip's catalog entry yet).
    if (!m_initialized || !m_textureUploader) return false;
    return m_textureUploader->prepareTexture(slot, width, height, format);
}

void D3D12Renderer::freeVideoTextureSlot(uint32_t slot) {
    if (!m_textureUploader) return;
    // The uploader can't wait on our fence — it's our responsibility to ensure
    // the GPU isn't using the slot's resources before we release them.
    waitForGpu();
    m_textureUploader->freeSlot(slot);
    std::cout << "Freed video texture slot " << slot << std::endl;
}

void D3D12Renderer::scheduleVideoTextureSlotFree(uint32_t slot) {
    // Safe to call from the editor thread (on_destroy<VideoTexture> observer).
    // The slot will be freed on the show thread at the top of beginShowFrame
    // after the show fence wait, ensuring the GPU is no longer sampling it.
    if (slot == UINT32_MAX) return;
    std::lock_guard<std::mutex> lk(m_deferredVideoTextureFreesMutex);
    m_deferredVideoTextureFrees.push_back(slot);
}

uint32_t D3D12Renderer::getVideoTextureSlotsAllocated() const {
    if (!m_textureUploader) return 0;
    return m_textureUploader->allocatedSlotCount();
}

bool D3D12Renderer::uploadVideoFrameToSlot(uint32_t slot,
                                           const uint8_t* data,
                                           uint32_t width, uint32_t height,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* outSrvHandle,
                                           TextureFormat format) {
    if (!m_initialized || !m_textureUploader) {
        return false;
    }

    // The uploader can't wait on our fence — if a resize or format change is
    // coming we need to ensure the GPU is done with the slot's previous
    // resources before we re-create them. waitForGpu drains both direct and
    // copy queues (Phase C.11).
    if (m_textureUploader->uploadWouldResize(slot, width, height, format) &&
        m_textureUploader->hasTexture(slot)) {
        waitForGpu();
    }

    // Phase C.11: record into the COPY queue's command list. endShowFrame()
    // executes it on the copy queue and inserts a cross-queue fence wait so
    // the direct queue doesn't sample the texture until the copy completes.
    //
    if (!m_textureUploader->upload(m_copyCommandList.Get(), slot, data, width, height, format)) {
        return false;
    }
    m_uploadsRecordedThisFrame = true;

    if (outSrvHandle) {
        *outSrvHandle = m_textureUploader->gpuHandle(slot);
    }
    return true;
}

// ============================================================================
// Textured Rendering Pipeline
// ============================================================================

Result D3D12Renderer::createTexturedRootSignature() {
    // Root parameters:
    // [0] Root constants for LayerConstants (b0) - 28 dwords = 112 bytes (M3 added uvRect)
    // [1] Descriptor table for texture SRV (t0)
    // Static sampler for texture sampling

    D3D12_ROOT_PARAMETER rootParameters[2] = {};

    // Parameter 0: Root constants (not CBV - ensures each draw gets its own values)
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[0].Constants.ShaderRegister = 0; // b0
    rootParameters[0].Constants.RegisterSpace = 0;
    rootParameters[0].Constants.Num32BitValues = 28; // sizeof(LayerConstants) / 4 (M3 ADR-0021)
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: Descriptor table for texture
    D3D12_DESCRIPTOR_RANGE descriptorRange = {};
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = 1;
    descriptorRange.BaseShaderRegister = 0; // t0
    descriptorRange.RegisterSpace = 0;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &descriptorRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler for texture sampling
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.MipLODBias = 0;
    staticSampler.MaxAnisotropy = 0;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0; // s0
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Define root signature
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 2;
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &staticSampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize root signature
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cerr << "Textured root signature serialization failed: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return Result::Failure;
    }

    // Create root signature
    hr = m_gpu->device()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_texturedRootSignature));
    if (FAILED(hr)) {
        std::cerr << "Failed to create textured root signature!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Textured root signature created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createTexturedPipelineState() {
    // Compile shaders
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    if (loadCompiledShader(L"shaders/composite_vs.cso", &vertexShader) != Result::Success) {
        std::cerr << "Failed to load composite_vs.cso (textured pipeline)!" << std::endl;
        return Result::Failure;
    }

    if (loadCompiledShader(L"shaders/composite_ps.cso", &pixelShader) != Result::Success) {
        std::cerr << "Failed to load composite_ps.cso!" << std::endl;
        return Result::Failure;
    }

    // Define vertex input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Define rasterizer state (shared by all blend modes)
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;

    // Base pipeline state description (shared by all blend modes)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_texturedRootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    // Composite-textured PSO base — Phase C.12 #3 FP16 compose target.
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr;

    // ========================================
    // Normal blend mode (standard alpha blending)
    // Result = Src * SrcAlpha + Dst * (1 - SrcAlpha)
    // ========================================
    D3D12_BLEND_DESC blendDescNormal = {};
    blendDescNormal.AlphaToCoverageEnable = FALSE;
    blendDescNormal.IndependentBlendEnable = FALSE;
    blendDescNormal.RenderTarget[0].BlendEnable = TRUE;
    blendDescNormal.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDescNormal.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDescNormal.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDescNormal.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDescNormal.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDescNormal.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDescNormal.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState = blendDescNormal;
    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineState));
    if (FAILED(hr)) {
        std::cerr << "Failed to create Normal blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    // ========================================
    // Add blend mode (additive blending)
    // Result = Src * SrcAlpha + Dst
    // ========================================
    D3D12_BLEND_DESC blendDescAdd = {};
    blendDescAdd.AlphaToCoverageEnable = FALSE;
    blendDescAdd.IndependentBlendEnable = FALSE;
    blendDescAdd.RenderTarget[0].BlendEnable = TRUE;
    blendDescAdd.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDescAdd.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    blendDescAdd.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDescAdd.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDescAdd.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blendDescAdd.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDescAdd.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState = blendDescAdd;
    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateAdd));
    if (FAILED(hr)) {
        std::cerr << "Failed to create Add blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    // ========================================
    // Multiply blend mode
    // Result = Src * Dst (darkens image), respecting alpha
    // Formula: lerp(Dest, Src * Dest, SrcAlpha) = Src * Dest * SrcAlpha + Dest * (1 - SrcAlpha)
    // The pixel shader outputs premultiplied RGB, so:
    // SrcBlend = DEST_COLOR multiplies source by destination
    // DestBlend = INV_SRC_ALPHA preserves background where source is transparent
    // ========================================
    D3D12_BLEND_DESC blendDescMultiply = {};
    blendDescMultiply.AlphaToCoverageEnable = FALSE;
    blendDescMultiply.IndependentBlendEnable = FALSE;
    blendDescMultiply.RenderTarget[0].BlendEnable = TRUE;
    blendDescMultiply.RenderTarget[0].SrcBlend = D3D12_BLEND_DEST_COLOR;
    blendDescMultiply.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDescMultiply.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDescMultiply.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDescMultiply.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDescMultiply.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDescMultiply.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState = blendDescMultiply;
    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateMultiply));
    if (FAILED(hr)) {
        std::cerr << "Failed to create Multiply blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    // ========================================
    // Screen blend mode
    // Result = 1 - (1 - Src) * (1 - Dst) = Src + Dst - Src * Dst (lightens image)
    // Using: Src * One + Dst * (1 - Src)
    // ========================================
    D3D12_BLEND_DESC blendDescScreen = {};
    blendDescScreen.AlphaToCoverageEnable = FALSE;
    blendDescScreen.IndependentBlendEnable = FALSE;
    blendDescScreen.RenderTarget[0].BlendEnable = TRUE;
    blendDescScreen.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendDescScreen.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    blendDescScreen.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDescScreen.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDescScreen.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDescScreen.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDescScreen.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState = blendDescScreen;
    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateScreen));
    if (FAILED(hr)) {
        std::cerr << "Failed to create Screen blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Textured pipeline states created (Normal, Add, Multiply, Screen)" << std::endl;
    return Result::Success;
}

// Root signature for the shader-blend pipeline. Same root constants + sampler
// as the textured root sig, but exposes two SRV descriptor tables instead of
// one (t0 = fg clip texture, t1 = compose-target snapshot for read-back).
Result D3D12Renderer::createBlendRootSignature() {
    D3D12_ROOT_PARAMETER rootParameters[3] = {};

    // [0] Root constants (b0) — LayerConstants, 28 dwords (M3 ADR-0021)
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[0].Constants.ShaderRegister = 0;
    rootParameters[0].Constants.RegisterSpace = 0;
    rootParameters[0].Constants.Num32BitValues = 28;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] fg SRV (t0)
    D3D12_DESCRIPTOR_RANGE fgRange = {};
    fgRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    fgRange.NumDescriptors = 1;
    fgRange.BaseShaderRegister = 0;
    fgRange.RegisterSpace = 0;
    fgRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &fgRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [2] bg snapshot SRV (t1)
    D3D12_DESCRIPTOR_RANGE bgRange = {};
    bgRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bgRange.NumDescriptors = 1;
    bgRange.BaseShaderRegister = 1;
    bgRange.RegisterSpace = 0;
    bgRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &bgRange;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Same static sampler (s0) as the textured pipeline.
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 3;
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &staticSampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cerr << "Blend root signature serialization failed: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return Result::Failure;
    }

    hr = m_gpu->device()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_blendRootSignature));
    if (FAILED(hr)) {
        std::cerr << "Failed to create blend root signature!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Blend root signature created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createBlendPipelineState() {
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    if (loadCompiledShader(L"shaders/composite_vs.cso", &vertexShader) != Result::Success) {
        std::cerr << "Failed to load composite_vs.cso (blend pipeline)!" << std::endl;
        return Result::Failure;
    }

    if (loadCompiledShader(L"shaders/composite_blend_ps.cso", &pixelShader) != Result::Success) {
        std::cerr << "Failed to load composite_blend_ps.cso!" << std::endl;
        return Result::Failure;
    }

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;

    // Shader writes the final composited color, so disable fixed-function blend.
    D3D12_BLEND_DESC blendDescReplace = {};
    blendDescReplace.AlphaToCoverageEnable = FALSE;
    blendDescReplace.IndependentBlendEnable = FALSE;
    blendDescReplace.RenderTarget[0].BlendEnable = FALSE;
    blendDescReplace.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_blendRootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDescReplace;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    // Shader-blend PSO (BlendEnable=FALSE) — also writes to FP16 compose.
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateBlend));
    if (FAILED(hr)) {
        std::cerr << "Failed to create blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Blend pipeline state created" << std::endl;
    return Result::Success;
}

// Modes that need the shader-blend pipeline (D3D12 fixed-function blend can't
// express any of these without a per-channel conditional or non-linear math).
static bool isShaderBlendMode(BlendMode mode) {
    switch (mode) {
        case BlendMode::Overlay:
        case BlendMode::SoftLight:
        case BlendMode::HardLight:
        case BlendMode::ColorDodge:
        case BlendMode::ColorBurn:
        case BlendMode::Darken:
        case BlendMode::Lighten:
        case BlendMode::Difference:
        case BlendMode::Exclusion:
            return true;
        default:
            return false;
    }
}

void D3D12Renderer::drawTexturedQuad(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                                     const DirectX::XMMATRIX& transform,
                                     float opacity,
                                     BlendMode blendMode,
                                     TextureColorSpace colorSpace,
                                     const std::string& ocioColorSpace,
                                     const DirectX::XMFLOAT4& uvRect) {
    if (!m_initialized || textureSrv.ptr == 0) {
        return;
    }

    // Phase C.12 #6 — per-call OCIO input processor selection. The renderer
    // looks up an input transform from the per-clip OCIO color-space name
    // and lazy-builds the matching PSO set. Empty name + no global default
    // fall back to the offline-compiled PSOs (no input transform).
    std::shared_ptr<const OcioGpuProcessor> activeInput;
    if (m_ocioManager && !ocioColorSpace.empty()) {
        activeInput = m_ocioManager->buildInputProcessor(ocioColorSpace, "ACEScg");
    }
    if (!activeInput) activeInput = m_activeInputProcessor;

    // Build constants structure with white color (texture provides the color)
    LayerConstants constants;
    DirectX::XMStoreFloat4x4(&constants.transform, DirectX::XMMatrixTranspose(transform));
    constants.color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constants.opacity = opacity;
    constants.blendMode = static_cast<uint32_t>(blendMode);  // Pass blend mode to shader
    constants.colorSpace = static_cast<uint32_t>(colorSpace); // 0=Linear, 1=YCoCg_scaled
    constants.padding3 = 0.0f;
    constants.uvRect = uvRect;  // ADR-0021 M3 Tiled source crop; identity = pre-M3 behavior

    // Ensure the shader-visible heap is bound regardless of which path we take.
    if (tl_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        tl_activeCmdList->SetDescriptorHeaps(1, heaps);
        tl_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    // ------------------------------------------------------------------------
    // Shader-blend modes: snapshot the current compose target into a separate
    // SRV so the pixel shader can sample it as the bg. Then run the blend PSO
    // (BlendEnable=FALSE) so the shader's output replaces the destination.
    // ------------------------------------------------------------------------
    // Phase C.12 #3: FP16 snapshot is allocated lazily on first shader-blend
    // draw. Workloads that never hit Overlay/Difference/etc. skip the ~33
    // MB-per-target allocation entirely.
    if (isShaderBlendMode(blendMode) &&
        m_currentComposeTargetSlot < m_composeTargets.size() &&
        ensureSnapshotResource(m_composeTargets[m_currentComposeTargetSlot])) {

        ComposeTarget& target = m_composeTargets[m_currentComposeTargetSlot];

        // RENDER_TARGET -> COPY_SOURCE on compose target, PIXEL_SHADER_RESOURCE
        // -> COPY_DEST on the snapshot, batched in one ResourceBarrier call.
        D3D12_RESOURCE_BARRIER pre[2] = {};
        pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[0].Transition.pResource = target.writeResource().Get();
        pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[1].Transition.pResource = target.snapshotResource.Get();
        pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tl_activeCmdList->ResourceBarrier(2, pre);

        tl_activeCmdList->CopyResource(target.snapshotResource.Get(), target.writeResource().Get());

        // Restore both resources to their pre-snapshot states. Compose target
        // returns to RENDER_TARGET so subsequent draws (this one + any later
        // ones in the same beginComposeTarget cycle) keep working.
        D3D12_RESOURCE_BARRIER post[2] = {};
        post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[0].Transition.pResource = target.writeResource().Get();
        post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        post[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        post[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[1].Transition.pResource = target.snapshotResource.Get();
        post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tl_activeCmdList->ResourceBarrier(2, post);

        // Phase C.12 #5/#6: pick OCIO-spliced shader-blend PSO if available
        // (uses the per-call OCIO color space resolved above).
        ID3D12PipelineState* blendPso = m_texturedPipelineStateBlend.Get();
        if (activeInput) {
            const OcioCompositePsoSet* set =
                getOrBuildOcioCompositePsoSet(*activeInput);
            if (set && set->psoBlend) blendPso = set->psoBlend.Get();
        }
        tl_activeCmdList->SetPipelineState(blendPso);
        tl_activeCmdList->SetGraphicsRootSignature(m_blendRootSignature.Get());
        tl_activeCmdList->SetGraphicsRoot32BitConstants(0, 28, &constants, 0);
        tl_activeCmdList->SetGraphicsRootDescriptorTable(1, textureSrv);            // fg (t0)
        tl_activeCmdList->SetGraphicsRootDescriptorTable(2, target.snapshotSrvHandle); // bg (t1)

        tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        tl_activeCmdList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        tl_activeCmdList->DrawInstanced(4, 1, 0, 0);
        return;
    }

    // ------------------------------------------------------------------------
    // Fixed-function blend modes: zero-copy, dispatched via per-mode PSOs.
    // Phase C.12 #5: when an OCIO input processor is bound, the OCIO-spliced
    // PSO set takes precedence over the offline-compiled PSOs.
    // ------------------------------------------------------------------------
    const OcioCompositePsoSet* ocioSet = nullptr;
    if (activeInput) {
        ocioSet = getOrBuildOcioCompositePsoSet(*activeInput);
    }

    ID3D12PipelineState* pipelineState = nullptr;
    if (ocioSet) {
        switch (blendMode) {
            case BlendMode::Add:      pipelineState = ocioSet->psoAdd.Get();      break;
            case BlendMode::Multiply: pipelineState = ocioSet->psoMultiply.Get(); break;
            case BlendMode::Screen:   pipelineState = ocioSet->psoScreen.Get();   break;
            default:                  pipelineState = ocioSet->psoNormal.Get();   break;
        }
    } else {
        switch (blendMode) {
            case BlendMode::Add:      pipelineState = m_texturedPipelineStateAdd.Get();      break;
            case BlendMode::Multiply: pipelineState = m_texturedPipelineStateMultiply.Get(); break;
            case BlendMode::Screen:   pipelineState = m_texturedPipelineStateScreen.Get();   break;
            default:                  pipelineState = m_texturedPipelineState.Get();         break;
        }
    }

    tl_activeCmdList->SetPipelineState(pipelineState);
    tl_activeCmdList->SetGraphicsRootSignature(m_texturedRootSignature.Get());
    tl_activeCmdList->SetGraphicsRoot32BitConstants(0, 28, &constants, 0);
    tl_activeCmdList->SetGraphicsRootDescriptorTable(1, textureSrv);

    // NOTE: Don't override viewport/scissor - caller (beginComposeTarget or beginShowFrame) sets these
    // This allows drawTexturedQuad to work correctly with both main window and offscreen targets

    tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    tl_activeCmdList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    tl_activeCmdList->DrawInstanced(4, 1, 0, 0);
}

// ============================================================================
// Mapping Surface Rendering Pipeline
// ============================================================================

Result D3D12Renderer::createMappingSurfaceRootSignature() {
    // Root parameters:
    // [0] CBV for MappingSurfaceConstants (b0)
    // [1] Descriptor table for texture SRV (t0)
    // Static sampler for texture sampling

    D3D12_ROOT_PARAMETER rootParameters[2] = {};

    // Parameter 0: Constant buffer
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0; // b0
    rootParameters[0].Descriptor.RegisterSpace = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: Descriptor table for texture
    D3D12_DESCRIPTOR_RANGE descriptorRange = {};
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = 1;
    descriptorRange.BaseShaderRegister = 0; // t0
    descriptorRange.RegisterSpace = 0;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &descriptorRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler for texture sampling
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.MipLODBias = 0;
    staticSampler.MaxAnisotropy = 0;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0; // s0
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Define root signature
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 2;
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &staticSampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize root signature
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cerr << "Mapping surface root signature serialization failed: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return Result::Failure;
    }

    // Create root signature
    hr = m_gpu->device()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_mappingSurfaceRootSignature));
    if (FAILED(hr)) {
        std::cerr << "Failed to create mapping surface root signature!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Mapping surface root signature created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createMappingSurfacePipelineState() {
    // Compile shaders
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    if (loadCompiledShader(L"shaders/mapping_surface_vs.cso", &vertexShader) != Result::Success) {
        std::cerr << "Failed to load mapping_surface_vs.cso!" << std::endl;
        return Result::Failure;
    }

    if (loadCompiledShader(L"shaders/mapping_surface_ps.cso", &pixelShader) != Result::Success) {
        std::cerr << "Failed to load mapping_surface_ps.cso!" << std::endl;
        return Result::Failure;
    }

    // Define vertex input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Define blend state for straight (non-premultiplied) alpha blending
    // FFmpeg outputs straight alpha, so we use SRC_ALPHA blending
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;  // Straight alpha
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Define rasterizer state
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;

    // Define pipeline state
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_mappingSurfaceRootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_mappingSurfacePipelineState));
    if (FAILED(hr)) {
        std::cerr << "Failed to create mapping surface pipeline state!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Mapping surface pipeline state created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createMappingSurfaceVertexBuffer() {
    // Define a unit quad with texture coordinates (0,0) to (1,1)
    // The shader will use these UV coordinates to interpolate between corners
    struct Vertex {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT2 texCoord;
    };

    // Two triangles forming a quad
    // Triangle 1: TL, TR, BR
    // Triangle 2: TL, BR, BL
    Vertex vertices[] = {
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },  // Top-left (index 0)
        { { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },  // Top-right (index 1)
        { { 1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },  // Bottom-right (index 2)
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },  // Top-left (index 0)
        { { 1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },  // Bottom-right (index 2)
        { { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } }   // Bottom-left (index 3)
    };

    const UINT vertexBufferSize = sizeof(vertices);

    // Create vertex buffer in upload heap
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = vertexBufferSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_mappingSurfaceVertexBuffer)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create mapping surface vertex buffer!" << std::endl;
        return Result::Failure;
    }
    m_mappingSurfaceVertexBuffer->SetName(L"MappingSurfaceVtxBuffer");

    // Copy vertex data to buffer
    void* vertexDataPtr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_mappingSurfaceVertexBuffer->Map(0, &readRange, &vertexDataPtr);
    if (FAILED(hr)) {
        std::cerr << "Failed to map mapping surface vertex buffer!" << std::endl;
        return Result::Failure;
    }

    memcpy(vertexDataPtr, vertices, vertexBufferSize);
    m_mappingSurfaceVertexBuffer->Unmap(0, nullptr);

    // Initialize vertex buffer view
    m_mappingSurfaceVertexBufferView.BufferLocation = m_mappingSurfaceVertexBuffer->GetGPUVirtualAddress();
    m_mappingSurfaceVertexBufferView.StrideInBytes = sizeof(Vertex);
    m_mappingSurfaceVertexBufferView.SizeInBytes = vertexBufferSize;

    std::cout << "Mapping surface vertex buffer created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createMappingSurfaceConstantBuffer() {
    // CRIT-04 fix: allocate a ring of per-draw constant buffer slots sized for
    // FRAME_COUNT frames-in-flight × MAX_MAPPING_SURFACES_PER_FRAME draws per frame.
    // Each frame occupies a contiguous region; within a frame each draw gets its
    // own 256-aligned slot. beginShowFrame() resets the draw index; moveToNextFrame()
    // fence-syncs a region before it's reused.
    m_mappingSurfaceSlotSize = (sizeof(MappingSurfaceConstants) + 255) & ~255u;
    const UINT totalSize = m_mappingSurfaceSlotSize * MAX_MAPPING_SURFACES_PER_FRAME * FRAME_COUNT;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = totalSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_mappingSurfaceConstantRing)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create mapping surface constant ring!" << std::endl;
        return Result::Failure;
    }
    m_mappingSurfaceConstantRing->SetName(L"MappingSurfaceCBRing");

    D3D12_RANGE readRange = { 0, 0 };
    void* mapped = nullptr;
    hr = m_mappingSurfaceConstantRing->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Failed to map mapping surface constant ring!" << std::endl;
        return Result::Failure;
    }
    m_mappingSurfaceConstantRingMapped = static_cast<uint8_t*>(mapped);

    std::cout << "Mapping surface constant ring created ("
              << MAX_MAPPING_SURFACES_PER_FRAME << " slots/frame × "
              << FRAME_COUNT << " frames, "
              << (totalSize / 1024) << " KB)" << std::endl;
    return Result::Success;
}

// ===========================================================================
// Per-layer effect chain (issue #54, PASS 1.5 of CompositorSystem).
// ===========================================================================

Result D3D12Renderer::createEffectRootSignature() {
    // Root parameters:
    // [0] Descriptor table for input texture SRV (t0)
    // [1] CBV for EffectParams (b0) — points at a slot in the per-frame ring.
    // Static sampler at s0 (LINEAR, CLAMP).
    //
    // Pixel-only SRV visibility (the shared VS is pure positional math).

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 0;  // b0
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;  // s0
    sampler.RegisterSpace  = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "Effect root sig serialize failed: "
                           << (const char*)err->GetBufferPointer() << std::endl;
        return Result::Failure;
    }

    hr = m_gpu->device()->CreateRootSignature(0, sig->GetBufferPointer(),
                                               sig->GetBufferSize(),
                                               IID_PPV_ARGS(&m_effectRootSignature));
    if (FAILED(hr)) {
        std::cerr << "Effect root sig create failed!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Effect root signature created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createEffectConstantBufferRing() {
    // Same pattern as createMappingSurfaceConstantBuffer — a persistently
    // mapped upload heap sized for FRAME_COUNT frames × per-frame budget ×
    // 256-byte cbuffer alignment. Each drawEffectPass advances
    // m_effectDrawIndex by one; beginShowFrame fence-syncs the previous
    // use of the current frame's region before reusing it.
    const UINT totalSize = EFFECT_CBUFFER_SLOT_SIZE *
                           MAX_EFFECT_DRAWS_PER_FRAME * FRAME_COUNT;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width  = totalSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_effectCbufferRing));
    if (FAILED(hr)) {
        std::cerr << "Failed to create effect cbuffer ring!" << std::endl;
        return Result::Failure;
    }
    m_effectCbufferRing->SetName(L"EffectCBRing");

    D3D12_RANGE readRange = { 0, 0 };
    void* mapped = nullptr;
    hr = m_effectCbufferRing->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Failed to map effect cbuffer ring!" << std::endl;
        return Result::Failure;
    }
    m_effectCbufferRingMapped = static_cast<uint8_t*>(mapped);

    std::cout << "Effect cbuffer ring created ("
              << MAX_EFFECT_DRAWS_PER_FRAME << " slots/frame × "
              << FRAME_COUNT << " frames, "
              << (totalSize / 1024) << " KB)" << std::endl;
    return Result::Success;
}

ID3D12PipelineState* D3D12Renderer::getOrBuildEffectPso(std::uint32_t kindIdHash) {
    if (auto it = m_effectPsoCache.find(kindIdHash); it != m_effectPsoCache.end()) {
        return it->second.Get();
    }
    if (!m_effectKindRegistry) {
        std::cerr << "[drawEffectPass] No EffectKindRegistry bound on renderer; "
                     "skipping kind 0x" << std::hex << kindIdHash << std::dec
                  << std::endl;
        return nullptr;
    }
    const effects::EffectKind* kind = m_effectKindRegistry->find(kindIdHash);
    if (!kind) {
        std::cerr << "[drawEffectPass] Unknown effect kind 0x"
                  << std::hex << kindIdHash << std::dec << std::endl;
        return nullptr;
    }

    // Load PS bytecode. User-authored kinds register their compiled
    // bytecode in-memory via RuntimeShaderCompiler (Phase 6 — see
    // EffectKindRegistry::scanUserEffects); engine kinds load their
    // `.cso` artifact from disk next to the executable.
    const std::uint8_t* psBytes = nullptr;
    std::size_t         psSize  = 0;
    ComPtr<ID3DBlob>    psBlob;  // owns the bytes when loaded from disk

    auto userView = m_effectKindRegistry->tryGetUserPsBytecode(kindIdHash);
    if (userView.valid()) {
        psBytes = userView.data;
        psSize  = userView.size;
    } else {
        std::wstring psPathW(kind->shaderPath.begin(), kind->shaderPath.end());
        if (loadCompiledShader(psPathW, &psBlob) != Result::Success) {
            std::cerr << "[drawEffectPass] Failed to load PS '" << kind->shaderPath
                      << "' for kind '" << kind->stableId << "'" << std::endl;
            return nullptr;
        }
        psBytes = static_cast<const std::uint8_t*>(psBlob->GetBufferPointer());
        psSize  = psBlob->GetBufferSize();
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_effectRootSignature.Get();
    pso.VS = { m_effectVsBlob->GetBufferPointer(), m_effectVsBlob->GetBufferSize() };
    pso.PS = { psBytes, psSize };

    // No input layout — VS reads SV_VertexID and synthesises a fullscreen
    // triangle. Disable blending (each effect writes a full RGBA replacement)
    // and depth (effects run before composite; no depth state in PASS 1.5).
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // matches ComposeTarget format
    pso.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> psoObj;
    HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&psoObj));
    if (FAILED(hr)) {
        std::cerr << "[drawEffectPass] CreateGraphicsPipelineState failed for "
                  << kind->stableId << " (0x" << std::hex << hr << std::dec
                  << ")" << std::endl;
        return nullptr;
    }

    auto* raw = psoObj.Get();
    m_effectPsoCache.emplace(kindIdHash, std::move(psoObj));
    return raw;
}

void D3D12Renderer::drawEffectPass(TextureRef input,
                                    std::uint32_t kindIdHash,
                                    const std::uint8_t* paramBlob,
                                    std::size_t paramBlobSize,
                                    std::uint32_t viewportWidth,
                                    std::uint32_t viewportHeight) {
    if (!m_initialized || !tl_activeCmdList) return;
    if (m_effectDrawIndex >= MAX_EFFECT_DRAWS_PER_FRAME) {
        if (!m_effectOverflowed) {
            std::cerr << "[drawEffectPass] Effect draw limit ("
                      << MAX_EFFECT_DRAWS_PER_FRAME
                      << "/frame) exceeded — additional effect passes dropped"
                      << std::endl;
            m_effectOverflowed = true;
        }
        return;
    }

    auto srv = resolveTextureHandle(input);
    if (srv.ptr == 0) return;  // Input texture not ready; skip the pass.

    ID3D12PipelineState* pso = getOrBuildEffectPso(kindIdHash);
    if (!pso) return;

    // Marshal the param blob into a per-frame cbuffer ring slot. Clamp the
    // copy to the slot size; the bake side promises ≤ 256 bytes.
    const uint32_t frameBase = m_currentBackBufferIndex * MAX_EFFECT_DRAWS_PER_FRAME;
    const uint32_t slotIndex = frameBase + m_effectDrawIndex;
    const uint32_t slotOffset = slotIndex * EFFECT_CBUFFER_SLOT_SIZE;
    const std::size_t copyBytes =
        (paramBlobSize > EFFECT_CBUFFER_SLOT_SIZE)
            ? EFFECT_CBUFFER_SLOT_SIZE
            : paramBlobSize;
    if (paramBlob && copyBytes > 0) {
        std::memcpy(m_effectCbufferRingMapped + slotOffset, paramBlob, copyBytes);
    }
    if (copyBytes < EFFECT_CBUFFER_SLOT_SIZE) {
        std::memset(m_effectCbufferRingMapped + slotOffset + copyBytes,
                    0, EFFECT_CBUFFER_SLOT_SIZE - copyBytes);
    }
    // Patch viewport size into the misc region (bytes 224-231 per
    // _effect_common.hlsli layout). The bake side doesn't know the target
    // RT dimensions, so the renderer writes them here.
    const float viewportFloats[2] = {
        static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight),
    };
    std::memcpy(m_effectCbufferRingMapped + slotOffset + 224,
                viewportFloats, sizeof(viewportFloats));

    const D3D12_GPU_VIRTUAL_ADDRESS cbufferGpuAddr =
        m_effectCbufferRing->GetGPUVirtualAddress() + slotOffset;

    // Bind shader-visible heap (the input SRV lives in the ImGui heap which
    // doubles as the compose-target + video-pool descriptor heap).
    if (tl_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        tl_activeCmdList->SetDescriptorHeaps(1, heaps);
        tl_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    tl_activeCmdList->SetPipelineState(pso);
    tl_activeCmdList->SetGraphicsRootSignature(m_effectRootSignature.Get());
    tl_activeCmdList->SetGraphicsRootDescriptorTable(0, srv);
    tl_activeCmdList->SetGraphicsRootConstantBufferView(1, cbufferGpuAddr);
    tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // No vertex / index buffer — VS synthesises positions from SV_VertexID.
    tl_activeCmdList->IASetVertexBuffers(0, 0, nullptr);
    tl_activeCmdList->DrawInstanced(3, 1, 0, 0);

    ++m_effectDrawIndex;
}

void D3D12Renderer::drawOutputSurface(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                                        const DirectX::XMFLOAT2 corners[4],
                                        const DirectX::XMFLOAT2 sourceUVs[4],
                                        const DirectX::XMFLOAT4& softEdges,
                                        float brightness,
                                        float gamma,
                                        float opacity) {
    if (!m_initialized || textureSrv.ptr == 0) {
        return;
    }

    // CRIT-04 fix: write to a unique per-draw slot in the ring buffer. Within a
    // single frame, each drawOutputSurface() call gets its own CB region so the
    // GPU reads consistent data at execute time even with multiple surfaces. Across
    // frames, the fence in moveToNextFrame() guarantees the CPU doesn't overwrite a
    // slot the GPU is still reading.
    if (m_mappingSurfaceDrawIndex >= MAX_MAPPING_SURFACES_PER_FRAME) {
        if (!m_mappingSurfaceOverflowed) {
            std::cerr << "[drawOutputSurface] Surface limit ("
                      << MAX_MAPPING_SURFACES_PER_FRAME
                      << "/frame) exceeded — additional surfaces dropped this frame" << std::endl;
            m_mappingSurfaceOverflowed = true;
        }
        return;
    }

    const uint32_t frameBase = m_currentBackBufferIndex * MAX_MAPPING_SURFACES_PER_FRAME;
    const uint32_t slotOffset = (frameBase + m_mappingSurfaceDrawIndex) * m_mappingSurfaceSlotSize;

    // Perspective-correct corner-pin: compute a per-corner `q` weight using
    // the classic q-trick (Heckbert). Given the 2D intersection I of the
    // quad's two diagonals (TL-BR and TR-BL), q_i = (d_i + d_opp) / d_opp
    // where d_X is the distance from corner X to I and d_opp is the same
    // distance for i's diagonal opposite. For a rectangle, all four q's
    // collapse to 2 (no-op). For a keystoned quad, the variation drives the
    // shader's projective divide, eliminating the diagonal texture fold.
    float q[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
    {
        const float ax = corners[2].x - corners[0].x;  // diag TL→BR
        const float ay = corners[2].y - corners[0].y;
        const float bx = corners[3].x - corners[1].x;  // diag TR→BL
        const float by = corners[3].y - corners[1].y;
        const float cx = corners[1].x - corners[0].x;
        const float cy = corners[1].y - corners[0].y;
        const float denom = ax * by - ay * bx;
        if (fabsf(denom) > 1e-8f) {
            const float s = (cx * by - cy * bx) / denom;
            const float Ix = corners[0].x + s * ax;
            const float Iy = corners[0].y + s * ay;
            auto dist = [&](int i) {
                const float dx = corners[i].x - Ix;
                const float dy = corners[i].y - Iy;
                return sqrtf(dx * dx + dy * dy);
            };
            const float d0 = dist(0), d1 = dist(1), d2 = dist(2), d3 = dist(3);
            const float eps = 1e-6f;
            q[0] = (d0 + d2) / std::max(d2, eps);
            q[1] = (d1 + d3) / std::max(d3, eps);
            q[2] = (d0 + d2) / std::max(d0, eps);
            q[3] = (d1 + d3) / std::max(d1, eps);
        }
        // denom ~= 0 means the two diagonals are parallel (degenerate /
        // collinear corners). Fallback to uniform q = 2 → identity mapping.
    }

    MappingSurfaceConstants constants;
    for (int i = 0; i < 4; ++i) {
        constants.corners[i] = DirectX::XMFLOAT4(corners[i].x, corners[i].y, 0.0f, 0.0f);
        // sourceUVs.w carries the per-corner projective weight q.
        constants.sourceUVs[i] = DirectX::XMFLOAT4(sourceUVs[i].x, sourceUVs[i].y, 0.0f, q[i]);
    }
    constants.softEdgeLeft = softEdges.x;
    constants.softEdgeRight = softEdges.y;
    constants.softEdgeTop = softEdges.z;
    constants.softEdgeBottom = softEdges.w;
    constants.brightness = brightness;
    constants.gamma = gamma;
    constants.opacity = opacity;
    constants.padding1 = 0.0f;

    memcpy(m_mappingSurfaceConstantRingMapped + slotOffset,
           &constants, sizeof(MappingSurfaceConstants));

    // Set mapping surface pipeline state. Phase C.12 #5: pick the OCIO-spliced
    // PSO when an active display processor is bound; otherwise fall back to
    // the offline-compiled gamma-only path.
    ID3D12PipelineState* mappingPso = m_mappingSurfacePipelineState.Get();
    if (m_activeDisplayProcessor) {
        auto p = getOrBuildOcioMappingSurfacePso(*m_activeDisplayProcessor);
        if (p) mappingPso = p.Get();
    }
    tl_activeCmdList->SetPipelineState(mappingPso);
    tl_activeCmdList->SetGraphicsRootSignature(m_mappingSurfaceRootSignature.Get());

    // Set descriptor heap (only if not already set)
    if (tl_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        tl_activeCmdList->SetDescriptorHeaps(1, heaps);
        tl_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    // Set root parameters — bind this draw's unique slot in the ring buffer
    const D3D12_GPU_VIRTUAL_ADDRESS slotGpuVa =
        m_mappingSurfaceConstantRing->GetGPUVirtualAddress() + slotOffset;
    tl_activeCmdList->SetGraphicsRootConstantBufferView(0, slotGpuVa);
    tl_activeCmdList->SetGraphicsRootDescriptorTable(1, textureSrv);

    m_mappingSurfaceDrawIndex++;

    // NOTE: Viewport/scissor are set by the caller (beginShowFrame,
    // beginComposeTarget, or beginOutputFrame). Overriding here to the main
    // back-buffer size was a latent bug that only manifested with per-output
    // rendering — a 1280x1080 secondary got clipped to the editor's 1920x1080
    // viewport, or vice-versa. Trust the caller's setup.

    // Set vertex buffer and draw (6 vertices = 2 triangles)
    tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    tl_activeCmdList->IASetVertexBuffers(0, 1, &m_mappingSurfaceVertexBufferView);
    tl_activeCmdList->DrawInstanced(6, 1, 0, 0);
}

// ========================================================================
// Compose Target (Offscreen Render Target for Multi-Clip Compositing)
// ========================================================================

uint32_t D3D12Renderer::createComposeTarget(uint32_t width, uint32_t height) {
    if (!m_initialized || !m_gpu) {
        std::cerr << "Cannot create compose target: renderer not initialized" << std::endl;
        return UINT32_MAX;
    }

    if (width == 0 || height == 0) {
        std::cerr << "Invalid compose target dimensions: " << width << "x" << height << std::endl;
        return UINT32_MAX;
    }

    // Hard-cap (#31). Beyond MAX_COMPOSE_TARGETS the descriptor-heap math
    // collides with VIDEO_TEXTURE_BASE — silent corruption that's almost
    // impossible to debug after the fact. Always reuse via resizeComposeTarget
    // instead of allocating fresh slots on dimension changes.
    if (m_composeTargets.size() >= IRenderer::MAX_COMPOSE_TARGETS) {
        std::cerr << "[D3D12] createComposeTarget: MAX_COMPOSE_TARGETS ("
                  << IRenderer::MAX_COMPOSE_TARGETS
                  << ") exhausted; resize an existing slot instead." << std::endl;
        return UINT32_MAX;
    }

    // No waitForGpu here. createComposeTarget runs on the show thread between
    // beginShowFrame and endShowFrame, with an open show command list. A drain
    // mid-frame races the editor's moveToNextFrame fence wait. We're creating
    // *new* resources at fresh descriptor heap slots that no prior GPU work
    // could reference, so a drain isn't required.

    // Create new compose target
    uint32_t slot = static_cast<uint32_t>(m_composeTargets.size());
    m_composeTargets.emplace_back();
    ComposeTarget& target = m_composeTargets.back();

    std::cout << "Creating compose target " << slot << ": " << width << "x" << height << std::endl;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    // Stage 3d: allocate all 3 triple-buffer sub-resources + RTVs + SRVs.
    for (uint32_t sub = 0; sub < ComposeTarget::TRIPLE; ++sub) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT hr = m_gpu->device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&target.rtvHeaps[sub]));
        if (FAILED(hr)) {
            std::cerr << "Failed to create compose target RTV heap [sub=" << sub << "]!" << std::endl;
            m_composeTargets.pop_back();
            return UINT32_MAX;
        }

        hr = m_gpu->device()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&target.resources[sub])
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create compose target texture [sub=" << sub << "]!" << std::endl;
            m_composeTargets.pop_back();
            return UINT32_MAX;
        }
        {
            wchar_t name[40];
            swprintf_s(name, L"ComposeTarget%u_sub%u", slot, sub);
            target.resources[sub]->SetName(name);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = target.rtvHeaps[sub]->GetCPUDescriptorHandleForHeapStart();
        m_gpu->device()->CreateRenderTargetView(target.resources[sub].Get(), &rtvDesc, rtvHandle);

        const uint32_t heapSlot = DescriptorHeapLayout::composeTargetTripleSlot(slot, sub);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = DescriptorHeapLayout::cpuHandle(
            m_imguiSrvHeap.Get(), heapSlot, m_srvDescriptorSize);
        m_gpu->device()->CreateShaderResourceView(target.resources[sub].Get(), &srvDesc, cpuHandle);
        target.srvHandles[sub] = DescriptorHeapLayout::gpuHandle(
            m_imguiSrvHeap.Get(), heapSlot, m_srvDescriptorSize);
    }

    // Snapshot texture is lazily allocated on first shader-blend draw.
    target.snapshotSrvSlot = DescriptorHeapLayout::snapshotSlot(slot);

    target.width  = width;
    target.height = height;
    target.writeIndex = 0;
    target.lastStableIndex.store(UINT32_MAX, std::memory_order_relaxed);
    target.ready  = true;

    // #75: publish the new target to editor-facing accessors only now that
    // it is fully initialized. The failure paths above pop_back without ever
    // publishing, so the editor can never observe a half-built element.
    m_composeTargetCount.store(static_cast<uint32_t>(m_composeTargets.size()),
                               std::memory_order_release);

    std::cout << "Compose target " << slot << " created successfully (triple-buffered): "
              << width << "x" << height << std::endl;

    return slot;
}

bool D3D12Renderer::resizeComposeTarget(uint32_t slot, uint32_t width, uint32_t height) {
    if (!m_initialized || !m_gpu) {
        std::cerr << "Cannot resize compose target: renderer not initialized" << std::endl;
        return false;
    }
    if (slot >= m_composeTargets.size()) {
        std::cerr << "[D3D12] resizeComposeTarget: invalid slot " << slot << std::endl;
        return false;
    }
    if (width == 0 || height == 0) {
        std::cerr << "[D3D12] resizeComposeTarget: invalid dimensions "
                  << width << "x" << height << std::endl;
        return false;
    }

    ComposeTarget& target = m_composeTargets[slot];

    // Same-size? No-op. Saves a wait + reallocation when CompositorSystem
    // calls us redundantly. If a deferred resize was pending and the dims
    // drifted back to current, cancel it — reopen the gate untouched.
    if (target.ready && target.width == width && target.height == height) {
        target.editorGate.open();
        return true;
    }

    // #75: editor ImGui frames grab this slot's SRV handles mid-record and
    // submit up to a frame later. Dropping the resources and rewriting the
    // descriptors while such a command list exists is a GPU use-after-free
    // (waitForGpu only drains work already on the queue). Close the gate —
    // editor readers skip this slot from their next isReadable() check — and
    // defer the swap until every editor frame begun before the close has
    // submitted. CompositorSystem retries every tick on false, so a deferral
    // just renders at the old dims for a tick or two; the editor preview
    // skips the slot for the same window.
    // Tell endShowFrame's liveness backstop this gate is actively driven
    // this tick (covers the deferral below AND the mid-rebuild failure
    // returns further down, both of which leave the gate closed on purpose).
    target.resizeAttemptedThisTick = true;

    target.editorGate.close(m_editorFrameTracker);
    if (!target.editorGate.mayMutate(m_editorFrameTracker)) {
        return false;
    }

    waitForGpu();

    // Mark not-ready so any concurrent render path (there shouldn't be one
    // post-waitForGpu, but be defensive) skips this slot until we're done.
    target.ready = false;

    // Drop all 3 triple-buffer GPU resources. RTV heaps and SRV slots in
    // m_imguiSrvHeap stay allocated — we re-bind them to the new resources.
    // The snapshot is sized to the texture, so it also has to go.
    for (uint32_t sub = 0; sub < ComposeTarget::TRIPLE; ++sub) {
        target.resources[sub].Reset();
    }
    target.snapshotResource.Reset();
    target.lastStableIndex.store(UINT32_MAX, std::memory_order_relaxed);

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width              = width;
    textureDesc.Height             = height;
    textureDesc.DepthOrArraySize   = 1;
    textureDesc.MipLevels          = 1;
    textureDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDesc.SampleDesc.Count   = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clearValue.Color[0]  = 0.0f;
    clearValue.Color[1]  = 0.0f;
    clearValue.Color[2]  = 0.0f;
    clearValue.Color[3]  = 1.0f;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format               = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtvDesc.ViewDimension        = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice   = 0;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                          = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels             = 1;
    srvDesc.Texture2D.MostDetailedMip       = 0;

    for (uint32_t sub = 0; sub < ComposeTarget::TRIPLE; ++sub) {
        HRESULT hr = m_gpu->device()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&target.resources[sub]));
        if (FAILED(hr)) {
            std::cerr << "[D3D12] resizeComposeTarget: CreateCommittedResource failed "
                      << "sub=" << sub << " hr=0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        {
            wchar_t name[40];
            swprintf_s(name, L"ComposeTarget%u_sub%u", slot, sub);
            target.resources[sub]->SetName(name);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            target.rtvHeaps[sub]->GetCPUDescriptorHandleForHeapStart();
        m_gpu->device()->CreateRenderTargetView(target.resources[sub].Get(), &rtvDesc, rtvHandle);

        const uint32_t heapSlot = DescriptorHeapLayout::composeTargetTripleSlot(slot, sub);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = DescriptorHeapLayout::cpuHandle(
            m_imguiSrvHeap.Get(), heapSlot, m_srvDescriptorSize);
        m_gpu->device()->CreateShaderResourceView(target.resources[sub].Get(), &srvDesc, cpuHandle);
        target.srvHandles[sub] = DescriptorHeapLayout::gpuHandle(
            m_imguiSrvHeap.Get(), heapSlot, m_srvDescriptorSize);
    }

    target.width  = width;
    target.height = height;
    target.ready  = true;

    // #75: reopen the gate — editor readers resume from their next frame.
    // On the CreateCommittedResource failure paths above the gate stays
    // closed (the target is torn down); CompositorSystem's per-tick retry
    // re-enters with mayMutate() already true and rebuilds it.
    target.editorGate.open();

    std::cout << "Resizing compose target " << slot << " to "
              << width << "x" << height << std::endl;
    return true;
}

void D3D12Renderer::beginComposeTarget(uint32_t slot) {
    if (slot >= m_composeTargets.size() || !m_composeTargets[slot].ready) {
        return;
    }

    ComposeTarget& target = m_composeTargets[slot];
    m_currentComposeTargetSlot = slot;

    // Transition the current write sub-resource from shader resource to render target.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.writeResource().Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tl_activeCmdList->ResourceBarrier(1, &barrier);

    // Get RTV handle for the current write sub-resource.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = target.writeRtvHeap()->GetCPUDescriptorHandleForHeapStart();

    // Set compose target as render target
    tl_activeCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear to black (compose target background)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    tl_activeCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Set viewport and scissor for compose target
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(target.width);
    viewport.Height = static_cast<float>(target.height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = {};
    scissorRect.right = target.width;
    scissorRect.bottom = target.height;

    tl_activeCmdList->RSSetViewports(1, &viewport);
    tl_activeCmdList->RSSetScissorRects(1, &scissorRect);
}

void D3D12Renderer::endComposeTarget() {
    if (m_currentComposeTargetSlot >= m_composeTargets.size() || !m_composeTargets[m_currentComposeTargetSlot].ready) {
        return;
    }

    ComposeTarget& target = m_composeTargets[m_currentComposeTargetSlot];

    // Transition the write sub-resource from render target to shader resource.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.writeResource().Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tl_activeCmdList->ResourceBarrier(1, &barrier);
}

// #75: common guard for the editor-thread accessors below. Bound-checks
// against the atomically-published count (never m_composeTargets.size(),
// which grows before the element is initialized), then checks the per-target
// gate. Reading `ready` after the gate's seq_cst load is race-free: any
// frame that can reach it either began before the gate closed (the show
// thread defers mutation until that frame submits) or observes the reopened
// gate after the mutation completed.
bool D3D12Renderer::composeTargetEditorReadable(uint32_t slot) const {
    if (slot >= m_composeTargetCount.load(std::memory_order_acquire)) return false;
    const ComposeTarget& target = m_composeTargets[slot];
    return target.editorGate.isReadable() && target.ready;
}

void* D3D12Renderer::getComposeTargetTextureID(uint32_t slot) const {
    if (!composeTargetEditorReadable(slot)) return nullptr;
    const D3D12_GPU_DESCRIPTOR_HANDLE h = m_composeTargets[slot].stableSrvHandle();
    return h.ptr ? reinterpret_cast<void*>(h.ptr) : nullptr;
}

void* D3D12Renderer::getVideoTextureIDForSlot(uint32_t slot) const {
    // ADR-0022 L5: ContentRoutingWindow uses this to draw a clip's
    // most-recently-uploaded frame under the feed-map region overlay.
    // TextureUploader owns the per-slot SRV descriptor; we just relay
    // its GPU handle. Returns nullptr until an upload has succeeded.
    if (!m_textureUploader) return nullptr;
    if (!m_textureUploader->hasTexture(slot)) return nullptr;
    const D3D12_GPU_DESCRIPTOR_HANDLE h = m_textureUploader->gpuHandle(slot);
    return h.ptr ? reinterpret_cast<void*>(h.ptr) : nullptr;
}

uint32_t D3D12Renderer::getComposeTargetStableSrvSlot(uint32_t slot) const {
    if (!composeTargetEditorReadable(slot)) return UINT32_MAX;
    const uint32_t stableIdx = m_composeTargets[slot].lastStableIndex.load(std::memory_order_acquire);
    if (stableIdx == UINT32_MAX) return UINT32_MAX;
    return DescriptorHeapLayout::composeTargetTripleSlot(slot, stableIdx % ComposeTarget::TRIPLE);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::getComposeTargetSrvHandle(uint32_t slot) const {
    if (composeTargetEditorReadable(slot)) {
        return m_composeTargets[slot].stableSrvHandle();
    }
    D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
    return nullHandle;
}

// Width/height are read by BOTH threads (CompositorSystem show-side each
// tick, StageWindow editor-side). Deliberately NOT gated on editorGate:
// during a deferred resize the old dims are intact and the show thread must
// keep rendering at them. Editor-side torn reads are excluded by the gate
// protocol as long as the caller checked a gated accessor (textureID /
// isComposeTargetReady) earlier in the same frame — which StageWindow and
// ProjectorCalibrationWindow both do.
uint32_t D3D12Renderer::getComposeTargetWidth(uint32_t slot) const {
    if (slot < m_composeTargetCount.load(std::memory_order_acquire)) {
        return m_composeTargets[slot].width;
    }
    return 0;
}

uint32_t D3D12Renderer::getComposeTargetHeight(uint32_t slot) const {
    if (slot < m_composeTargetCount.load(std::memory_order_acquire)) {
        return m_composeTargets[slot].height;
    }
    return 0;
}

// Editor-thread readiness probe (StageWindow). Show-side code checks the
// plain `ready` member directly — it must NOT see false during a deferred
// resize, while the editor must.
bool D3D12Renderer::isComposeTargetReady(uint32_t slot) const {
    return composeTargetEditorReadable(slot);
}

// ============================================================================
// Output Windows (physical display outputs) — Phase C #1
// ============================================================================

uint32_t D3D12Renderer::createOutputWindow(const char* title,
                                            int32_t x, int32_t y,
                                            uint32_t width, uint32_t height) {
    if (!m_initialized || !m_gpu || !m_gpu->isInitialized() || !m_dxgiFactory) {
        std::cerr << "[OutputWindow] Renderer not initialized" << std::endl;
        return UINT32_MAX;
    }

    // Create a borderless GLFW window that takes no focus and ignores input.
    // Projection surface, not an editor — it exists to show pixels and nothing else.
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // Position first, then show.

    GLFWwindow* win = glfwCreateWindow(
        static_cast<int>(width),
        static_cast<int>(height),
        title ? title : "Entity Output",
        nullptr, nullptr);

    if (!win) {
        std::cerr << "[OutputWindow] glfwCreateWindow failed" << std::endl;
        return UINT32_MAX;
    }

    glfwSetWindowPos(win, x, y);
    glfwShowWindow(win);

    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) {
        std::cerr << "[OutputWindow] glfwGetWin32Window returned null" << std::endl;
        glfwDestroyWindow(win);
        return UINT32_MAX;
    }

    OutputWindow slot;
    slot.window = win;
    slot.hwnd = hwnd;
    slot.width = width;
    slot.height = height;
    slot.active = true;

    // Create a swap chain on this HWND, sharing the main command queue and factory.
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FRAME_COUNT;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = m_dxgiFactory->CreateSwapChainForHwnd(
        m_gpu->commandQueue(),
        hwnd,
        &desc,
        nullptr, nullptr,
        &swapChain1);
    if (FAILED(hr)) {
        std::cerr << "[OutputWindow] CreateSwapChainForHwnd failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        glfwDestroyWindow(win);
        return UINT32_MAX;
    }
    if (FAILED(swapChain1.As(&slot.swapChain))) {
        std::cerr << "[OutputWindow] IDXGISwapChain3 cast failed" << std::endl;
        glfwDestroyWindow(win);
        return UINT32_MAX;
    }

    // Suppress alt-enter fullscreen on this secondary window (DXGI default).
    m_dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // RTV heap + views (one per back buffer).
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = m_gpu->device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&slot.rtvHeap));
    if (FAILED(hr)) {
        std::cerr << "[OutputWindow] RTV heap creation failed" << std::endl;
        glfwDestroyWindow(win);
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = slot.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        hr = slot.swapChain->GetBuffer(i, IID_PPV_ARGS(&slot.renderTargets[i]));
        if (FAILED(hr)) {
            std::cerr << "[OutputWindow] GetBuffer " << i << " failed" << std::endl;
            glfwDestroyWindow(win);
            return UINT32_MAX;
        }
        {
            wchar_t name[40];
            swprintf_s(name, L"OutputSwapChainBuf%u", i);
            slot.renderTargets[i]->SetName(name);
        }
        m_gpu->device()->CreateRenderTargetView(slot.renderTargets[i].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    slot.currentBackBufferIndex = slot.swapChain->GetCurrentBackBufferIndex();

    // Find a free slot (freed via destroyOutputWindow) or append.
    uint32_t slotIndex = UINT32_MAX;
    for (uint32_t i = 0; i < m_outputWindows.size(); ++i) {
        if (!m_outputWindows[i].active && !m_outputWindows[i].window) {
            m_outputWindows[i] = std::move(slot);
            slotIndex = i;
            break;
        }
    }
    if (slotIndex == UINT32_MAX) {
        slotIndex = static_cast<uint32_t>(m_outputWindows.size());
        m_outputWindows.push_back(std::move(slot));
    }

    std::cout << "[OutputWindow] Created slot " << slotIndex
              << " at (" << x << "," << y << ") "
              << width << "x" << height
              << " hwnd=0x" << std::hex << reinterpret_cast<uintptr_t>(hwnd) << std::dec
              << std::endl;
    return slotIndex;
}

void D3D12Renderer::destroyOutputWindow(uint32_t outputSlot) {
    if (outputSlot >= m_outputWindows.size()) return;
    OutputWindow& ow = m_outputWindows[outputSlot];
    if (!ow.active) return;

    // Wait for GPU so we don't yank back buffers still in flight.
    waitForGpu();

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        ow.renderTargets[i].Reset();
    }
    ow.rtvHeap.Reset();
    ow.swapChain.Reset();

    if (ow.window) {
        glfwDestroyWindow(ow.window);
        ow.window = nullptr;
    }
    ow.hwnd = nullptr;
    ow.active = false;
    ow.width = 0;
    ow.height = 0;
    ow.currentBackBufferIndex = 0;

    std::cout << "[OutputWindow] Destroyed slot " << outputSlot << std::endl;
}

void D3D12Renderer::resizeOutputWindow(uint32_t outputSlot,
                                        uint32_t width, uint32_t height) {
    if (outputSlot >= m_outputWindows.size()) return;
    OutputWindow& ow = m_outputWindows[outputSlot];
    if (!ow.active || !ow.swapChain) return;
    if (width == 0 || height == 0) return;
    if (width == ow.width && height == ow.height) return;

    waitForGpu();

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        ow.renderTargets[i].Reset();
    }

    HRESULT hr = ow.swapChain->ResizeBuffers(
        FRAME_COUNT, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        std::cerr << "[OutputWindow] ResizeBuffers failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = ow.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (FAILED(ow.swapChain->GetBuffer(i, IID_PPV_ARGS(&ow.renderTargets[i])))) {
            std::cerr << "[OutputWindow] GetBuffer after resize failed" << std::endl;
            return;
        }
        {
            wchar_t name[48];
            swprintf_s(name, L"OutputSwapChainBuf%u_slot%u", i, outputSlot);
            ow.renderTargets[i]->SetName(name);
        }
        m_gpu->device()->CreateRenderTargetView(ow.renderTargets[i].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    ow.width = width;
    ow.height = height;
    ow.currentBackBufferIndex = ow.swapChain->GetCurrentBackBufferIndex();
}

uint32_t D3D12Renderer::getOutputWindowWidth(uint32_t outputSlot) const {
    if (outputSlot >= m_outputWindows.size()) return 0;
    const OutputWindow& ow = m_outputWindows[outputSlot];
    return ow.active ? ow.width : 0;
}

uint32_t D3D12Renderer::getOutputWindowHeight(uint32_t outputSlot) const {
    if (outputSlot >= m_outputWindows.size()) return 0;
    const OutputWindow& ow = m_outputWindows[outputSlot];
    return ow.active ? ow.height : 0;
}

void D3D12Renderer::beginOutputFrame(uint32_t outputSlot) {
    if (outputSlot >= m_outputWindows.size()) return;
    OutputWindow& ow = m_outputWindows[outputSlot];
    if (!ow.active) return;

    // Transition this back buffer to RENDER_TARGET.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = ow.renderTargets[ow.currentBackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tl_activeCmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ow.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += ow.currentBackBufferIndex * m_rtvDescriptorSize;
    tl_activeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(ow.width);
    viewport.Height = static_cast<float>(ow.height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor = {};
    scissor.right = static_cast<LONG>(ow.width);
    scissor.bottom = static_cast<LONG>(ow.height);

    tl_activeCmdList->RSSetViewports(1, &viewport);
    tl_activeCmdList->RSSetScissorRects(1, &scissor);

    m_currentOutputSlot = outputSlot;
}

void D3D12Renderer::clearOutputFrame(uint32_t outputSlot,
                                      float r, float g, float b, float a) {
    if (outputSlot >= m_outputWindows.size()) return;
    OutputWindow& ow = m_outputWindows[outputSlot];
    if (!ow.active) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ow.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += ow.currentBackBufferIndex * m_rtvDescriptorSize;
    const float color[] = { r, g, b, a };
    tl_activeCmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void D3D12Renderer::endOutputFrame(uint32_t outputSlot) {
    if (outputSlot >= m_outputWindows.size()) return;
    OutputWindow& ow = m_outputWindows[outputSlot];
    if (!ow.active) return;

    // Transition output back buffer to PRESENT state.
    // No back-buffer restore here — the show command list must not touch
    // the editor-owned back buffer. beginEditorFrame sets up the back-buffer
    // RTV on the editor command list.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = ow.renderTargets[ow.currentBackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tl_activeCmdList->ResourceBarrier(1, &barrier);

    m_currentOutputSlot = UINT32_MAX;
}

// ============================================================================
// Screenshot Capture Implementation
// ============================================================================

bool D3D12Renderer::ensureScreenshotStagingBuffer(uint32_t width, uint32_t height) {
    // Check if existing buffer is sufficient
    if (m_screenshotStagingBuffer &&
        m_screenshotStagingWidth >= width &&
        m_screenshotStagingHeight >= height) {
        return true;
    }

    // Wait for GPU before modifying resources
    waitForGpu();
    m_screenshotStagingBuffer.Reset();

    // Calculate row pitch (must be 256-byte aligned for D3D12)
    uint64_t rowPitch = (static_cast<uint64_t>(width) * 4 + 255) & ~255ULL;
    uint64_t bufferSize = rowPitch * height;

    // Create readback buffer
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.SampleDesc.Quality = 0;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_screenshotStagingBuffer)
    );

    if (FAILED(hr)) {
        std::cerr << "[Screenshot] Failed to create staging buffer! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    m_screenshotStagingWidth = width;
    m_screenshotStagingHeight = height;
    m_screenshotStagingRowPitch = rowPitch;

    std::cout << "[Screenshot] Created staging buffer: " << width << "x" << height
              << " (row pitch: " << rowPitch << ")" << std::endl;

    return true;
}

bool D3D12Renderer::readbackTextureToPixels(ID3D12Resource* sourceTexture,
                                             D3D12_RESOURCE_STATES sourceState,
                                             uint32_t width, uint32_t height,
                                             std::vector<uint8_t>& outPixels) {
    if (!sourceTexture) {
        std::cerr << "[Readback] Source texture is null!" << std::endl;
        return false;
    }

    if (!ensureScreenshotStagingBuffer(width, height)) {
        return false;
    }

    // Wait for any previous GPU work
    waitForGpu();

    // Reset command allocator and command list for this operation
    HRESULT hr = m_editorAllocators[m_currentBackBufferIndex]->Reset();
    if (FAILED(hr)) {
        std::cerr << "[Readback] Failed to reset command allocator!" << std::endl;
        return false;
    }

    hr = m_editorCmdList->Reset(m_editorAllocators[m_currentBackBufferIndex].Get(), nullptr);
    if (FAILED(hr)) {
        std::cerr << "[Readback] Failed to reset command list!" << std::endl;
        return false;
    }

    // Transition source texture to COPY_SOURCE if needed
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.StateBefore = sourceState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_editorCmdList->ResourceBarrier(1, &barrier);
    }

    // Set up copy locations
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = sourceTexture;
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = m_screenshotStagingBuffer.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLocation.PlacedFootprint.Offset = 0;
    dstLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstLocation.PlacedFootprint.Footprint.Width = width;
    dstLocation.PlacedFootprint.Footprint.Height = height;
    dstLocation.PlacedFootprint.Footprint.Depth = 1;
    dstLocation.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(m_screenshotStagingRowPitch);

    // Copy texture to staging buffer
    m_editorCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    // Transition source texture back to original state
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = sourceState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_editorCmdList->ResourceBarrier(1, &barrier);
    }

    // Close and execute command list
    hr = m_editorCmdList->Close();
    if (FAILED(hr)) {
        std::cerr << "[Readback] Failed to close command list!" << std::endl;
        return false;
    }

    ID3D12CommandList* commandLists[] = { m_editorCmdList.Get() };
    m_gpu->commandQueue()->ExecuteCommandLists(1, commandLists);

    // Wait for GPU to finish the copy
    waitForGpu();

    // Map staging buffer and read pixels
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(m_screenshotStagingRowPitch * height) };
    hr = m_screenshotStagingBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        std::cerr << "[Readback] Failed to map staging buffer! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    // Copy data to a contiguous buffer, removing row pitch padding
    outPixels.resize(static_cast<size_t>(width) * height * 4);
    const uint8_t* src = static_cast<uint8_t*>(mappedData);
    uint8_t* dst = outPixels.data();
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst, src, width * 4);
        src += m_screenshotStagingRowPitch;
        dst += width * 4;
    }

    // Unmap
    D3D12_RANGE writeRange = { 0, 0 }; // We didn't write
    m_screenshotStagingBuffer->Unmap(0, &writeRange);

    return true;
}

bool D3D12Renderer::readbackTextureToPNG(ID3D12Resource* sourceTexture,
                                          D3D12_RESOURCE_STATES sourceState,
                                          uint32_t width, uint32_t height,
                                          const std::string& filepath) {
    std::vector<uint8_t> pixels;
    if (!readbackTextureToPixels(sourceTexture, sourceState, width, height, pixels)) {
        return false;
    }

    // Create output directory if needed
    std::filesystem::path path(filepath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    // Write PNG file
    int result = stbi_write_png(
        filepath.c_str(),
        static_cast<int>(width),
        static_cast<int>(height),
        4,  // RGBA
        pixels.data(),
        static_cast<int>(width * 4)
    );

    if (result == 0) {
        std::cerr << "[Screenshot] Failed to write PNG: " << filepath << std::endl;
        return false;
    }

    std::cout << "[Screenshot] Saved: " << filepath << " (" << width << "x" << height << ")" << std::endl;
    return true;
}

bool D3D12Renderer::captureComposeTargetToPNG(const std::string& filepath, uint32_t slot) {
    // Phase C.12 #3: route through the tone-mapping capture pass so the
    // saved PNG represents "what an sRGB monitor would show" rather than
    // raw FP16 linear values. Subtask 5 swaps the sRGB stub in
    // aces_capture_ps for an OCIO-emitted display transform.
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    if (!tonemapAndReadbackComposeTarget(slot, width, height, pixels)) return false;

    std::filesystem::path path(filepath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    int result = stbi_write_png(filepath.c_str(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                4,
                                pixels.data(),
                                static_cast<int>(width * 4));
    if (result == 0) {
        std::cerr << "[Screenshot] Failed to write PNG: " << filepath << std::endl;
        return false;
    }
    std::cout << "[Screenshot] Saved: " << filepath
              << " (" << width << "x" << height << ")" << std::endl;
    return true;
}

bool D3D12Renderer::readComposeTargetPixels(uint32_t slot,
                                             uint32_t& outWidth,
                                             uint32_t& outHeight,
                                             std::vector<uint8_t>& outPixels) {
    // Phase C.12 #3: tone-mapping capture pass keeps the readback bytes
    // portable across machines (vs hashing raw FP16 quantization). The
    // golden tests under tests/goldens/**/*.hash hash these bytes.
    return tonemapAndReadbackComposeTarget(slot, outWidth, outHeight, outPixels);
}

bool D3D12Renderer::captureBackBufferToPNG(const std::string& filepath) {
    if (!m_initialized) {
        std::cerr << "[Screenshot] Renderer not initialized!" << std::endl;
        return false;
    }

    // Note: Back buffer should be in PRESENT state after endEditorFrame()
    // We capture from the current back buffer
    return readbackTextureToPNG(
        m_renderTargets[m_currentBackBufferIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        m_width,
        m_height,
        filepath
    );
}

// ============================================================================
// IRenderer virtual overrides (abstract-typed delegators)
//
// These resolve TextureRef / glm types into D3D12 handles and matrices, then
// forward to the existing D3D12-typed overloads. Once Phase B task 18 migrates
// all external callers to these, the D3D12-typed overloads are removed.
// ============================================================================

namespace {

inline DirectX::XMMATRIX glmToXm(const glm::mat4& m) {
    // Both glm::mat4 and DirectX::XMMATRIX are column-major. Match the
    // element-by-element conversion already used in CompositorSystem.
    return DirectX::XMMatrixSet(
        m[0][0], m[0][1], m[0][2], m[0][3],
        m[1][0], m[1][1], m[1][2], m[1][3],
        m[2][0], m[2][1], m[2][2], m[2][3],
        m[3][0], m[3][1], m[3][2], m[3][3]
    );
}

} // anonymous namespace

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::resolveTextureHandle(TextureRef tex) const {
    switch (tex.kind) {
        case TextureRef::Kind::VideoSlot:
            if (m_textureUploader && m_textureUploader->hasTexture(tex.slot)) {
                return m_textureUploader->gpuHandle(tex.slot);
            }
            break;
        case TextureRef::Kind::ComposeTarget:
            if (tex.slot < m_composeTargets.size() && m_composeTargets[tex.slot].ready) {
                return m_composeTargets[tex.slot].stableSrvHandle();
            }
            break;
        case TextureRef::Kind::Invalid:
        default:
            break;
    }
    return D3D12_GPU_DESCRIPTOR_HANDLE{};  // .ptr == 0 — drawTexturedQuad etc. no-op on this
}

bool D3D12Renderer::uploadVideoFrameToSlot(uint32_t slot,
                                            const uint8_t* rgba,
                                            uint32_t width,
                                            uint32_t height) {
    D3D12_GPU_DESCRIPTOR_HANDLE discard{};
    return uploadVideoFrameToSlot(slot, rgba, width, height, &discard,
                                  TextureFormat::RGBA8_UNORM);
}

bool D3D12Renderer::uploadVideoFrameToSlot(uint32_t slot,
                                            const uint8_t* data,
                                            uint32_t width,
                                            uint32_t height,
                                            TextureFormat format) {
    D3D12_GPU_DESCRIPTOR_HANDLE discard{};
    return uploadVideoFrameToSlot(slot, data, width, height, &discard, format);
}

bool D3D12Renderer::uploadVideoFrameToSlotImmediate(uint32_t slot,
                                                    const uint8_t* rgba,
                                                    uint32_t width,
                                                    uint32_t height) {
    if (!m_initialized || !m_textureUploader) {
        return false;
    }

    // A resize / format change must wait for the GPU to finish with the
    // slot's previous resources before they are re-created.
    if (m_textureUploader->uploadWouldResize(slot, width, height,
                                             TextureFormat::RGBA8_UNORM) &&
        m_textureUploader->hasTexture(slot)) {
        waitForGpu();
    }

    // Record into the editor-owned copy list, NOT m_copyCommandList. The
    // show thread Resets m_copyCommandList every beginShowFrame, so a copy
    // recorded here from the editor thread would be discarded before it
    // executes. Dedicated editor copy resources + immediate execute mirror
    // uploadMeshImmediate. See ADR-0014.
    const uint32_t ci = m_currentBackBufferIndex;
    HRESULT hr = m_editorCopyCommandAllocators[ci]->Reset();
    if (FAILED(hr)) {
        std::cerr << "[D3D12Renderer] editor copy allocator Reset failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    hr = m_editorCopyCommandList->Reset(m_editorCopyCommandAllocators[ci].Get(), nullptr);
    if (FAILED(hr)) {
        std::cerr << "[D3D12Renderer] editor copy list Reset failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    if (!m_textureUploader->upload(m_editorCopyCommandList.Get(), slot,
                                   rgba, width, height,
                                   TextureFormat::RGBA8_UNORM)) {
        m_editorCopyCommandList->Close();
        return false;
    }

    hr = m_editorCopyCommandList->Close();
    if (FAILED(hr)) {
        std::cerr << "[D3D12Renderer] editor copy list Close failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    ID3D12CommandList* copyLists[] = { m_editorCopyCommandList.Get() };
    m_gpu->copyQueue()->ExecuteCommandLists(1, copyLists);
    const uint64_t copyValue = ++m_uploadFenceValue;
    m_gpu->copyQueue()->Signal(m_uploadFence.Get(), copyValue);
    // Cross-queue wait: the direct queue must not sample the texture until
    // the copy queue has finished writing it.
    m_gpu->commandQueue()->Wait(m_uploadFence.Get(), copyValue);
    return true;
}

bool D3D12Renderer::copyUploadBufferToVideoTextureSlot(
        uint32_t slot,
        UploadHeapBuffer* buf,
        uint32_t width,
        uint32_t height,
        TextureFormat format) {
    if (!m_initialized || !m_textureUploader || !buf || !buf->resource) {
        return false;
    }
    // Must be called from the show thread inside a beginShowFrame/endShowFrame
    // pair — tl_activeCmdList is the open show command list.
    if (!tl_activeCmdList) {
        std::cerr << "D3D12Renderer::copyUploadBufferToVideoTextureSlot: called outside show frame\n";
        return false;
    }

    // Guard against resize / format change: if ensureTexture inside
    // recordDirectCopy would recreate the texture, the old GPU resources
    // must be idle first.
    //
    // In steady state (post-prepareVideoTextureSlot at project load) this never
    // fires — the texture is pre-allocated at the correct dimensions. The path
    // exists as a safety net for mid-stream resolution changes. waitForGpu()
    // blocks the show thread for up to the fence timeout (2 s); acceptable only
    // because it is expected to be extremely rare (resolution change mid-clip).
    if (m_textureUploader->uploadWouldResize(slot, width, height, format) &&
        m_textureUploader->hasTexture(slot)) {
        waitForGpu();
    }

    // Record CopyTextureRegion from the UploadHeapBuffer into the slot's
    // DEFAULT-heap texture on the DIRECT (show) queue's command list.
    //
    // Why DIRECT queue, not COPY queue:
    //   The show thread samples the texture immediately after via the compositor
    //   draw calls in the SAME command list. If CopyTextureRegion were on the
    //   COPY queue, we'd need a cross-queue fence Wait on the DIRECT queue before
    //   the draw — exactly what the per-frame upload() path does (lines 619-624).
    //   Using the same command list avoids that overhead and is semantically
    //   correct: the copy and the draw are sequenced by command-list order on a
    //   single queue.
    //
    // Implicit state promotion (D3D12 spec, Table D.1):
    //   UPLOAD-heap source (COMMON) → GENERIC_READ promoted implicitly on DIRECT.
    //   DEFAULT-heap destination (COMMON) → COPY_DEST promoted implicitly on DIRECT.
    //   Both decay back to COMMON at ExecuteCommandLists boundary.
    //   No explicit ResourceBarrier calls needed.
    //
    // m_uploadsRecordedThisFrame is NOT set here — that flag controls the COPY
    // queue path in endShowFrame (lines 619-624). This path uses the show list
    // directly and needs no separate Execute/Signal/Wait sequence.
    if (!m_textureUploader->recordDirectCopy(tl_activeCmdList,
                                              slot,
                                              buf->resource.Get(),
                                              buf->footprint,
                                              width, height, format)) {
        return false;
    }

    // Stamp the fence value so the UploadHeapBuffer's shared_ptr deleter
    // fence-waits before recycling. The DIRECT queue signals m_showFence with
    // m_showFenceValues[m_showBackBufferIndex] at the end of endShowFrame
    // (line ~658). That signal covers both the CopyTextureRegion recorded above
    // AND the compositor draws that follow — so the buffer is safe to recycle
    // once that value is completed.
    if (m_uploadHeapPool) {
        const uint64_t fenceValue = m_showFenceValues[m_showBackBufferIndex];
        m_uploadHeapPool->recordCopyDone(*buf, fenceValue);
    }
    return true;
}

TextureRef D3D12Renderer::getVideoTexture(uint32_t slot) const {
    if (m_textureUploader && m_textureUploader->isAllocated(slot)) {
        return TextureRef::video(slot);
    }
    return TextureRef::invalid();
}

TextureRef D3D12Renderer::getComposeTargetTexture(uint32_t slot) const {
    if (slot < m_composeTargets.size() && m_composeTargets[slot].ready) {
        return TextureRef::compose(slot);
    }
    return TextureRef::invalid();
}

void D3D12Renderer::drawColoredQuad(const glm::mat4& transform,
                                     const glm::vec4& color,
                                     float opacity) {
    drawColoredQuad(glmToXm(transform),
                    DirectX::XMFLOAT4(color.r, color.g, color.b, color.a),
                    opacity);
}

void D3D12Renderer::drawTexturedQuad(TextureRef texture,
                                      const glm::mat4& transform,
                                      float opacity,
                                      BlendMode blendMode,
                                      TextureColorSpace colorSpace,
                                      const std::string& ocioColorSpace,
                                      const glm::vec4& uvRect) {
    const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveTextureHandle(texture);
    if (srv.ptr == 0) return;  // Invalid or unready texture — drop silently
    const DirectX::XMFLOAT4 xmUvRect{uvRect.x, uvRect.y, uvRect.z, uvRect.w};
    drawTexturedQuad(srv, glmToXm(transform), opacity, blendMode, colorSpace, ocioColorSpace, xmUvRect);
}

void D3D12Renderer::drawOutputSurface(TextureRef texture,
                                        const glm::vec2 corners[4],
                                        const glm::vec2 sourceUVs[4],
                                        const glm::vec4& softEdges,
                                        float brightness,
                                        float gamma,
                                        float opacity) {
    const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveTextureHandle(texture);
    if (srv.ptr == 0) return;

    DirectX::XMFLOAT2 xmCorners[4];
    DirectX::XMFLOAT2 xmUVs[4];
    for (int i = 0; i < 4; ++i) {
        xmCorners[i] = DirectX::XMFLOAT2(corners[i].x, corners[i].y);
        xmUVs[i]     = DirectX::XMFLOAT2(sourceUVs[i].x, sourceUVs[i].y);
    }
    const DirectX::XMFLOAT4 xmSoft(softEdges.x, softEdges.y, softEdges.z, softEdges.w);

    drawOutputSurface(srv, xmCorners, xmUVs, xmSoft, brightness, gamma, opacity);
}

// =============================================================================
// Phase C.12 #3 — lazy snapshot allocation + capture/tone-mapping pass
// =============================================================================

bool D3D12Renderer::ensureSnapshotResource(ComposeTarget& target) {
    if (target.snapshotResource) return true;
    if (!target.writeResource() || target.width == 0 || target.height == 0) return false;

    D3D12_RESOURCE_DESC snapshotDesc = target.writeResource()->GetDesc();
    snapshotDesc.Flags = D3D12_RESOURCE_FLAG_NONE; // No RTV needed on the snapshot

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &snapshotDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(&target.snapshotResource));
    if (FAILED(hr)) {
        std::cerr << "[Capture] ensureSnapshotResource: CreateCommittedResource failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    {
        wchar_t name[40];
        swprintf_s(name, L"ComposeSnapshot_srv%u", target.snapshotSrvSlot);
        target.snapshotResource->SetName(name);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                        = snapshotDesc.Format;
    srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels           = 1;
    srvDesc.Texture2D.MostDetailedMip     = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = DescriptorHeapLayout::cpuHandle(
        m_imguiSrvHeap.Get(), target.snapshotSrvSlot, m_srvDescriptorSize);
    m_gpu->device()->CreateShaderResourceView(target.snapshotResource.Get(), &srvDesc, cpu);
    target.snapshotSrvHandle = DescriptorHeapLayout::gpuHandle(
        m_imguiSrvHeap.Get(), target.snapshotSrvSlot, m_srvDescriptorSize);
    return true;
}

bool D3D12Renderer::createCaptureRootSignatureAndPSO() {
    // Root signature: one SRV (the FP16 compose target) + static sampler.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                     = 1;
    srvRange.BaseShaderRegister                 = 0;
    srvRange.RegisterSpace                      = 0;
    srvRange.OffsetInDescriptorsFromTableStart  = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParam.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 1;
    rsDesc.pParameters       = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "[Capture] root sig serialize: "
                           << static_cast<const char*>(err->GetBufferPointer()) << std::endl;
        return false;
    }
    hr = m_gpu->device()->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&m_captureRootSignature));
    if (FAILED(hr)) {
        std::cerr << "[Capture] CreateRootSignature failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    ComPtr<ID3DBlob> vs, ps;
    if (loadCompiledShader(L"shaders/aces_capture_vs.cso", &vs) != Result::Success) {
        std::cerr << "[Capture] missing shaders/aces_capture_vs.cso" << std::endl;
        return false;
    }
    if (loadCompiledShader(L"shaders/aces_capture_ps.cso", &ps) != Result::Success) {
        std::cerr << "[Capture] missing shaders/aces_capture_ps.cso" << std::endl;
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature  = m_captureRootSignature.Get();
    psoDesc.VS              = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS              = { ps->GetBufferPointer(), ps->GetBufferSize() };

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE; // full-screen tri
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask                      = UINT_MAX;
    psoDesc.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets                = 1;
    psoDesc.RTVFormats[0]                   = DXGI_FORMAT_R8G8B8A8_UNORM; // capture output is UNORM8
    psoDesc.SampleDesc.Count                = 1;

    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_capturePipelineState));
    if (FAILED(hr)) {
        std::cerr << "[Capture] CreateGraphicsPipelineState failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    return true;
}

bool D3D12Renderer::ensureCaptureResource(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (m_captureResource && m_captureWidth >= width && m_captureHeight >= height) {
        return true;
    }

    waitForGpu();
    m_captureResource.Reset();

    if (!m_captureRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HRESULT hr = m_gpu->device()->CreateDescriptorHeap(&rtvHeapDesc,
                                                          IID_PPV_ARGS(&m_captureRtvHeap));
        if (FAILED(hr)) {
            std::cerr << "[Capture] CreateDescriptorHeap(RTV) failed hr=0x"
                      << std::hex << hr << std::dec << std::endl;
            return false;
        }
        m_captureRtv = m_captureRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width              = width;
    desc.Height             = height;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear.Color[3] = 1.0f;

    HRESULT hr = m_gpu->device()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
        IID_PPV_ARGS(&m_captureResource));
    if (FAILED(hr)) {
        std::cerr << "[Capture] capture resource alloc failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    m_captureResource->SetName(L"CaptureResource");

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_gpu->device()->CreateRenderTargetView(m_captureResource.Get(), &rtvDesc, m_captureRtv);

    m_captureWidth  = width;
    m_captureHeight = height;
    return true;
}

bool D3D12Renderer::tonemapAndReadbackComposeTarget(uint32_t slot,
                                                    uint32_t& outWidth,
                                                    uint32_t& outHeight,
                                                    std::vector<uint8_t>& outPixels) {
    if (slot >= m_composeTargets.size() || !m_composeTargets[slot].ready) {
        std::cerr << "[Capture] compose target " << slot << " not ready" << std::endl;
        return false;
    }
    const ComposeTarget& target = m_composeTargets[slot];
    outWidth  = target.width;
    outHeight = target.height;

    if (!ensureCaptureResource(target.width, target.height)) return false;
    if (!ensureScreenshotStagingBuffer(target.width, target.height)) return false;

    waitForGpu();

    HRESULT hr = m_editorAllocators[m_currentBackBufferIndex]->Reset();
    if (FAILED(hr)) {
        std::cerr << "[Capture] command allocator Reset failed" << std::endl;
        return false;
    }
    hr = m_editorCmdList->Reset(m_editorAllocators[m_currentBackBufferIndex].Get(), nullptr);
    if (FAILED(hr)) {
        std::cerr << "[Capture] command list Reset failed" << std::endl;
        return false;
    }

    // 1. Bind the SRV heap, viewport, RTV, and run the tone-map pass.
    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    m_editorCmdList->SetDescriptorHeaps(1, heaps);
    tl_currentDescriptorHeap = m_imguiSrvHeap.Get();

    D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height) };
    m_editorCmdList->RSSetViewports(1, &vp);
    m_editorCmdList->RSSetScissorRects(1, &scissor);
    m_editorCmdList->OMSetRenderTargets(1, &m_captureRtv, FALSE, nullptr);

    // Phase C.12 #5: prefer the OCIO-spliced capture PSO so the captured
    // image matches what mapping_surface_ps puts on a sRGB monitor. Falls
    // back to the linearToSrgb stub from C.12 #3 when OCIO isn't bound.
    ID3D12PipelineState* capturePso = m_ocioCapturePipelineState
        ? m_ocioCapturePipelineState.Get()
        : m_capturePipelineState.Get();
    m_editorCmdList->SetPipelineState(capturePso);
    m_editorCmdList->SetGraphicsRootSignature(m_captureRootSignature.Get());
    m_editorCmdList->SetGraphicsRootDescriptorTable(0, target.stableSrvHandle());

    m_editorCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_editorCmdList->DrawInstanced(3, 1, 0, 0); // full-screen triangle from SV_VERTEXID

    // 2. RENDER_TARGET -> COPY_SOURCE, copy to staging, restore.
    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource   = m_captureResource.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_editorCmdList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource         = m_captureResource.Get();
    src.Type              = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex  = 0;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource                              = m_screenshotStagingBuffer.Get();
    dst.Type                                   = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset                 = 0;
    dst.PlacedFootprint.Footprint.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width        = target.width;
    dst.PlacedFootprint.Footprint.Height       = target.height;
    dst.PlacedFootprint.Footprint.Depth        = 1;
    dst.PlacedFootprint.Footprint.RowPitch     = static_cast<UINT>(m_screenshotStagingRowPitch);

    // m_captureResource is reused across compose sizes (sized to the LARGEST
    // requested), so we need an explicit srcBox bounded to the current
    // compose target — passing nullptr would imply the full capture
    // resource subresource and trigger E_INVALIDARG on the second screen
    // when sizes differ.
    D3D12_BOX srcBox = { 0, 0, 0, target.width, target.height, 1 };
    m_editorCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);

    D3D12_RESOURCE_BARRIER restore = {};
    restore.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore.Transition.pResource   = m_captureResource.Get();
    restore.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    restore.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    restore.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_editorCmdList->ResourceBarrier(1, &restore);

    hr = m_editorCmdList->Close();
    if (FAILED(hr)) {
        std::cerr << "[Capture] command list Close failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    ID3D12CommandList* lists[] = { m_editorCmdList.Get() };
    m_gpu->commandQueue()->ExecuteCommandLists(1, lists);
    waitForGpu();

    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(m_screenshotStagingRowPitch * target.height) };
    hr = m_screenshotStagingBuffer->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        std::cerr << "[Capture] staging Map failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    outPixels.resize(static_cast<size_t>(target.width) * target.height * 4);
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped);
    uint8_t*       dstBytes = outPixels.data();
    for (uint32_t y = 0; y < target.height; ++y) {
        memcpy(dstBytes, srcBytes, static_cast<size_t>(target.width) * 4);
        srcBytes += m_screenshotStagingRowPitch;
        dstBytes += target.width * 4;
    }

    D3D12_RANGE writeRange = { 0, 0 };
    m_screenshotStagingBuffer->Unmap(0, &writeRange);
    return true;
}

// =============================================================================
// Phase C.12 #5 — OCIO PSO splicing + runtime DXC compile + draw-time selection.
// (entity-namespace includes + std headers live at the top of this file —
// dropping them here put std symbols inside the entity namespace.)
// =============================================================================

namespace {

// Splice OCIO's HLSL function source onto a base pixel shader source.
// Returns the concatenated HLSL ready to feed RuntimeShaderCompiler. The
// `-D ENTITY_OCIO_*_FN=<name>` macro is passed as a separate compile flag,
// not embedded here.
std::string spliceOcioFunction(const std::string& ocioFunctionSource,
                               const std::string& baseShaderSource) {
    // OCIO emits a self-contained block of static const arrays + helper
    // functions + the main fn. We just prepend it. The base shader's
    // #include "common.hlsli" stays as-is and DXC resolves it via the
    // include dir we pass.
    std::string out;
    out.reserve(ocioFunctionSource.size() + baseShaderSource.size() + 64);
    out += "// === OCIO-emitted transform (Phase C.12 #5) ===\n";
    out += ocioFunctionSource;
    out += "\n// === host shader follows ===\n";
    out += baseShaderSource;
    return out;
}

// Build the standard `D` flag DXC consumes to define the macro.
std::wstring makeDxcDefineFlag(const std::string& macro, const std::string& value) {
    std::string s = "-D" + macro + "=" + value;
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

} // anonymous namespace

std::string D3D12Renderer::loadShaderSourceFromExeDir(const std::string& filename) {
    namespace fs = std::filesystem;
    fs::path exeDir;
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) exeDir = fs::path(buf).parent_path();
    }
    fs::path full = exeDir / "shaders" / filename;
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        std::cerr << "[OCIO/PSO] missing shader source: " << full.string() << "\n";
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool D3D12Renderer::loadOcioShaderSources() {
    m_compositePsSource       = loadShaderSourceFromExeDir("composite_ps.hlsl");
    m_compositeBlendPsSource  = loadShaderSourceFromExeDir("composite_blend_ps.hlsl");
    m_mappingSurfacePsSource  = loadShaderSourceFromExeDir("mapping_surface_ps.hlsl");
    m_acesCapturePsSource     = loadShaderSourceFromExeDir("aces_capture_ps.hlsl");
    return !m_compositePsSource.empty()
        && !m_compositeBlendPsSource.empty()
        && !m_mappingSurfacePsSource.empty()
        && !m_acesCapturePsSource.empty();
}

void D3D12Renderer::setOcioManager(OcioManager* mgr) {
    if (m_ocioManager == mgr) return;
    waitForGpu();

    m_ocioManager = mgr;
    m_ocioCompositePsoCache.clear();
    m_ocioMappingSurfacePsoCache.clear();
    m_ocioCapturePipelineState.Reset();
    m_activeInputProcessor.reset();
    m_activeDisplayProcessor.reset();
    m_activeCaptureProcessor.reset();

    if (!mgr) return;

    if (!loadOcioShaderSources()) {
        std::cerr << "[OCIO/PSO] could not load shader source files; OCIO PSO "
                     "splice disabled, falling back to offline-compiled paths\n";
        m_ocioManager = nullptr;
        return;
    }

    // Pre-bake the default input + display + capture PSOs so the first
    // frame doesn't pay a runtime DXC cost.
    // Working space is hard-coded to ACEScg until subtask 7 makes it a
    // Settings field.
    auto inputProc = mgr->buildInputProcessor("Linear Rec.709 (sRGB)", "ACEScg");
    if (inputProc) {
        m_activeInputProcessor = inputProc;
        if (!getOrBuildOcioCompositePsoSet(*inputProc)) {
            std::cerr << "[OCIO/PSO] failed to build default input PSO set\n";
        }
    }

    const std::string display = mgr->getDefaultDisplay();
    const std::string view    = mgr->getDefaultView(display);
    if (!display.empty() && !view.empty()) {
        auto dispProc = mgr->buildDisplayProcessor("ACEScg", display, view);
        if (dispProc) {
            m_activeDisplayProcessor = dispProc;
            if (!getOrBuildOcioMappingSurfacePso(*dispProc)) {
                std::cerr << "[OCIO/PSO] failed to build default mapping_surface PSO\n";
            }
        }
    }

    // Capture PSO is pinned to the canonical "sRGB output" pair so the
    // golden tests hash a portable image regardless of whether the user
    // configures a fancier display+view for the live output.
    // Use the same display+view we picked for the live output if it's the
    // sRGB one; otherwise look for the sRGB Display.
    std::string capDisplay, capView;
    for (const auto& d : mgr->listDisplays()) {
        if (d.find("sRGB") != std::string::npos) {
            capDisplay = d;
            for (const auto& v : mgr->listViews(d)) {
                if (v.find("ACES") != std::string::npos) { capView = v; break; }
            }
            if (capView.empty() && !mgr->listViews(d).empty()) {
                capView = mgr->getDefaultView(d);
            }
            break;
        }
    }
    if (capDisplay.empty()) { capDisplay = display; capView = view; }

    if (!capDisplay.empty() && !capView.empty()) {
        auto capProc = mgr->buildDisplayProcessor("ACEScg", capDisplay, capView);
        if (capProc) {
            m_activeCaptureProcessor = capProc;
            m_ocioCapturePipelineState = buildOcioCapturePso(*capProc);
            if (!m_ocioCapturePipelineState) {
                std::cerr << "[OCIO/PSO] failed to build OCIO capture PSO; "
                             "falling back to sRGB-stub\n";
            }
        }
    }
}

const D3D12Renderer::OcioCompositePsoSet*
D3D12Renderer::getOrBuildOcioCompositePsoSet(const OcioGpuProcessor& input) {
    const std::string& key = input.functionName();
    auto it = m_ocioCompositePsoCache.find(key);
    if (it != m_ocioCompositePsoCache.end()) return &it->second;

    namespace fs = std::filesystem;
    wchar_t exeBuf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path includeDir = (n > 0 && n < MAX_PATH)
                            ? fs::path(exeBuf).parent_path() / L"shaders"
                            : fs::path();
    std::vector<std::wstring> includeDirs{ includeDir.wstring() };
    std::vector<std::wstring> defines{ makeDxcDefineFlag("ENTITY_OCIO_INPUT_FN", input.functionName()) };

    const std::string compositeSrc      = spliceOcioFunction(input.shaderText(), m_compositePsSource);
    const std::string compositeBlendSrc = spliceOcioFunction(input.shaderText(), m_compositeBlendPsSource);

    auto compositeBlob      = m_runtimeCompiler.compileAndCache(compositeSrc, "PSMain", "ps_6_0", includeDirs, defines);
    auto compositeBlendBlob = m_runtimeCompiler.compileAndCache(compositeBlendSrc, "PSMain", "ps_6_0", includeDirs, defines);
    if (!compositeBlob.success || !compositeBlendBlob.success) {
        if (!compositeBlob.success)
            std::cerr << "[OCIO/PSO] composite_ps compile failed for '" << key
                      << "':\n" << compositeBlob.errors << "\n";
        if (!compositeBlendBlob.success)
            std::cerr << "[OCIO/PSO] composite_blend_ps compile failed for '" << key
                      << "':\n" << compositeBlendBlob.errors << "\n";
        return nullptr;
    }

    // Re-use the existing offline VS — only the PS changes for OCIO.
    ComPtr<ID3DBlob> vsBlob;
    if (loadCompiledShader(L"shaders/composite_vs.cso", &vsBlob) != Result::Success) {
        std::cerr << "[OCIO/PSO] missing composite_vs.cso\n";
        return nullptr;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    auto makePsoBase = [&](ID3D12RootSignature* rs,
                           const D3D12_BLEND_DESC& blend,
                           IDxcBlob* psBlob,
                           ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = rs;
        d.InputLayout = { inputLayout, _countof(inputLayout) };
        d.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        d.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.BlendState = blend;
        d.DepthStencilState.DepthEnable = FALSE;
        d.DepthStencilState.StencilEnable = FALSE;
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // FP16 compose target
        d.SampleDesc.Count = 1;
        HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&outPso));
        if (FAILED(hr)) {
            std::cerr << "[OCIO/PSO] CreateGraphicsPipelineState failed hr=0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
        return true;
    };

    // Blend descs for the four fixed-function modes — pulled from the
    // existing offline createTexturedPipelineState bodies. Kept inline
    // here so the OCIO PSO builder is self-contained.
    auto rtNormal = []{
        D3D12_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return b;
    }();
    auto rtAdd = []{
        D3D12_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        b.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        b.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return b;
    }();
    auto rtMultiply = []{
        D3D12_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D12_BLEND_DEST_COLOR;
        b.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return b;
    }();
    auto rtScreen = []{
        D3D12_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
        b.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        b.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return b;
    }();
    auto rtReplace = []{
        D3D12_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = FALSE;
        b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return b;
    }();

    OcioCompositePsoSet set;
    bool ok = true;
    ok &= makePsoBase(m_texturedRootSignature.Get(), rtNormal,    compositeBlob.blob.Get(),       set.psoNormal);
    ok &= makePsoBase(m_texturedRootSignature.Get(), rtAdd,       compositeBlob.blob.Get(),       set.psoAdd);
    ok &= makePsoBase(m_texturedRootSignature.Get(), rtMultiply,  compositeBlob.blob.Get(),       set.psoMultiply);
    ok &= makePsoBase(m_texturedRootSignature.Get(), rtScreen,    compositeBlob.blob.Get(),       set.psoScreen);
    ok &= makePsoBase(m_blendRootSignature.Get(),    rtReplace,   compositeBlendBlob.blob.Get(),  set.psoBlend);
    if (!ok) {
        std::cerr << "[OCIO/PSO] partial PSO build failure for '" << key << "'\n";
        return nullptr;
    }

    auto [insertedIt, _] = m_ocioCompositePsoCache.try_emplace(key, std::move(set));
    return &insertedIt->second;
}

ComPtr<ID3D12PipelineState>
D3D12Renderer::getOrBuildOcioMappingSurfacePso(const OcioGpuProcessor& display) {
    const std::string& key = display.functionName();
    auto it = m_ocioMappingSurfacePsoCache.find(key);
    if (it != m_ocioMappingSurfacePsoCache.end()) return it->second;

    namespace fs = std::filesystem;
    wchar_t exeBuf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path includeDir = (n > 0 && n < MAX_PATH)
                            ? fs::path(exeBuf).parent_path() / L"shaders"
                            : fs::path();
    std::vector<std::wstring> includeDirs{ includeDir.wstring() };
    std::vector<std::wstring> defines{ makeDxcDefineFlag("ENTITY_OCIO_DISPLAY_FN", display.functionName()) };

    const std::string src = spliceOcioFunction(display.shaderText(), m_mappingSurfacePsSource);
    auto blob = m_runtimeCompiler.compileAndCache(src, "PSMain", "ps_6_0", includeDirs, defines);
    if (!blob.success) {
        std::cerr << "[OCIO/PSO] mapping_surface_ps compile failed for '" << key
                  << "':\n" << blob.errors << "\n";
        return {};
    }

    ComPtr<ID3DBlob> vsBlob;
    if (loadCompiledShader(L"shaders/mapping_surface_vs.cso", &vsBlob) != Result::Success) {
        std::cerr << "[OCIO/PSO] missing mapping_surface_vs.cso\n";
        return {};
    }

    // Mirror the offline mapping-surface PSO blend state — straight alpha.
    D3D12_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable        = TRUE;
    bd.RenderTarget[0].SrcBlend           = D3D12_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend          = D3D12_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp            = D3D12_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha      = D3D12_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha     = D3D12_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha       = D3D12_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = m_mappingSurfaceRootSignature.Get();
    d.InputLayout = { inputLayout, _countof(inputLayout) };
    d.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    d.PS = { blob.blob->GetBufferPointer(), blob.blob->GetBufferSize() };
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.BlendState = bd;
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.StencilEnable = FALSE;
    d.SampleMask = UINT_MAX;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // mapping_surface writes to swap chain
    d.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        std::cerr << "[OCIO/PSO] mapping_surface CreateGraphicsPipelineState failed hr=0x"
                  << std::hex << hr << std::dec << "\n";
        return {};
    }

    m_ocioMappingSurfacePsoCache.emplace(key, pso);
    return pso;
}

ComPtr<ID3D12PipelineState>
D3D12Renderer::buildOcioCapturePso(const OcioGpuProcessor& display) {
    namespace fs = std::filesystem;
    wchar_t exeBuf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path includeDir = (n > 0 && n < MAX_PATH)
                            ? fs::path(exeBuf).parent_path() / L"shaders"
                            : fs::path();
    std::vector<std::wstring> includeDirs{ includeDir.wstring() };
    std::vector<std::wstring> defines{ makeDxcDefineFlag("ENTITY_OCIO_DISPLAY_FN", display.functionName()) };

    const std::string src = spliceOcioFunction(display.shaderText(), m_acesCapturePsSource);
    auto psBlob = m_runtimeCompiler.compileAndCache(src, "PSMain", "ps_6_0", includeDirs, defines);
    if (!psBlob.success) {
        std::cerr << "[OCIO/PSO] aces_capture_ps compile failed for '" << display.functionName()
                  << "':\n" << psBlob.errors << "\n";
        return {};
    }

    ComPtr<ID3DBlob> vsBlob;
    if (loadCompiledShader(L"shaders/aces_capture_vs.cso", &vsBlob) != Result::Success) {
        std::cerr << "[OCIO/PSO] missing aces_capture_vs.cso\n";
        return {};
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature       = m_captureRootSignature.Get();
    d.VS                   = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    d.PS                   = { psBlob.blob->GetBufferPointer(), psBlob.blob->GetBufferSize() };
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.DepthStencilState.DepthEnable = FALSE;
    d.SampleMask = UINT_MAX;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // capture target is UNORM8
    d.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_gpu->device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        std::cerr << "[OCIO/PSO] capture CreateGraphicsPipelineState failed hr=0x"
                  << std::hex << hr << std::dec << "\n";
        return {};
    }
    return pso;
}

// =============================================================================
// Projector calibration overlay
// =============================================================================

bool D3D12Renderer::createCalibrationOverlayPSO() {
    // Root signature: one root CBV (b0) for the CalibrationConstants struct.
    // No SRV, no sampler — the PS draws crosshairs procedurally.
    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParam.Constants.ShaderRegister  = 0;
    rootParam.Constants.RegisterSpace   = 0;
    rootParam.Constants.Num32BitValues  = sizeof(CalibrationConstants) / 4;
    rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &rootParam;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS  |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS    |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS  |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "[CalibOverlay] root sig: "
                           << static_cast<const char*>(err->GetBufferPointer()) << "\n";
        return false;
    }
    hr = m_gpu->device()->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&m_calibRootSignature));
    if (FAILED(hr)) { std::cerr << "[CalibOverlay] CreateRootSignature hr=0x" << std::hex << hr << "\n"; return false; }

    ComPtr<ID3DBlob> vs, ps;
    if (loadCompiledShader(L"shaders/calibration_overlay_vs.cso", &vs) != Result::Success) {
        std::cerr << "[CalibOverlay] missing calibration_overlay_vs.cso\n"; return false;
    }
    if (loadCompiledShader(L"shaders/calibration_overlay_ps.cso", &ps) != Result::Success) {
        std::cerr << "[CalibOverlay] missing calibration_overlay_ps.cso\n"; return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature  = m_calibRootSignature.Get();
    psoDesc.VS              = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS              = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable         = FALSE;
    psoDesc.DepthStencilState.StencilEnable       = FALSE;
    psoDesc.SampleMask                            = UINT_MAX;
    psoDesc.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets                      = 1;
    psoDesc.RTVFormats[0]                         = DXGI_FORMAT_R8G8B8A8_UNORM; // output swap chain backbuffer format
    psoDesc.SampleDesc.Count                      = 1;
    // No InputLayout — the VS uses SV_VertexID (no vertex buffer)

    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_calibPipelineState));
    if (FAILED(hr)) {
        std::cerr << "[CalibOverlay] CreateGraphicsPipelineState hr=0x" << std::hex << hr << "\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Mesh-triangle PSO — proper barycentric UV interpolation for projector output.
// ---------------------------------------------------------------------------

bool D3D12Renderer::createMeshTrianglePSO() {
    // Root signature:
    //  [0] 32-bit constants (b0): 6 × float4 = 24 DWORDs (3 verts + 3 UVs)
    //  [1] descriptor table for texture SRV (t0)
    //  static sampler s0
    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace  = 0;
    rootParams[0].Constants.Num32BitValues = 24;  // 3*4 verts + 3*4 uvs
    rootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[1].ParameterType                          = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges    = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges      = &srvRange;
    rootParams[1].ShaderVisibility                       = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 2;
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS    |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS  |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "[MeshTri] root sig: "
                           << static_cast<const char*>(err->GetBufferPointer()) << "\n";
        return false;
    }
    hr = m_gpu->device()->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&m_meshTriRootSignature));
    if (FAILED(hr)) { std::cerr << "[MeshTri] CreateRootSignature hr=0x" << std::hex << hr << "\n"; return false; }

    ComPtr<ID3DBlob> vs, ps;
    if (loadCompiledShader(L"shaders/mesh_triangle_vs.cso", &vs) != Result::Success) {
        std::cerr << "[MeshTri] missing mesh_triangle_vs.cso\n"; return false;
    }
    if (loadCompiledShader(L"shaders/mesh_triangle_ps.cso", &ps) != Result::Success) {
        std::cerr << "[MeshTri] missing mesh_triangle_ps.cso\n"; return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature  = m_meshTriRootSignature.Get();
    psoDesc.VS              = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS              = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE; // CPU does backface check
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable         = FALSE;
    psoDesc.DepthStencilState.StencilEnable       = FALSE;
    psoDesc.SampleMask                            = UINT_MAX;
    psoDesc.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets                      = 1;
    psoDesc.RTVFormats[0]                         = DXGI_FORMAT_R8G8B8A8_UNORM; // output-frame format
    psoDesc.SampleDesc.Count                      = 1;

    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_meshTriPipelineState));
    if (FAILED(hr)) {
        std::cerr << "[MeshTri] CreateGraphicsPipelineState hr=0x" << std::hex << hr << "\n";
        return false;
    }
    std::cout << "Mesh-triangle PSO created\n";
    return true;
}

void D3D12Renderer::drawMeshTriangle(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                                      const glm::vec2 verts[3],
                                      const glm::vec2 uvs[3]) {
    if (!m_initialized || textureSrv.ptr == 0) return;
    if (!m_meshTriRootSignature || !m_meshTriPipelineState) {
        if (!createMeshTrianglePSO()) return;
    }

    // Pack constants: 6 × float4 = 24 DWORDs
    float constants[24];
    for (int i = 0; i < 3; ++i) {
        constants[i * 4 + 0] = verts[i].x;
        constants[i * 4 + 1] = verts[i].y;
        constants[i * 4 + 2] = 0.0f;
        constants[i * 4 + 3] = 0.0f;
    }
    for (int i = 0; i < 3; ++i) {
        constants[12 + i * 4 + 0] = uvs[i].x;
        constants[12 + i * 4 + 1] = uvs[i].y;
        constants[12 + i * 4 + 2] = 0.0f;
        constants[12 + i * 4 + 3] = 0.0f;
    }

    tl_activeCmdList->SetPipelineState(m_meshTriPipelineState.Get());
    tl_activeCmdList->SetGraphicsRootSignature(m_meshTriRootSignature.Get());
    tl_activeCmdList->SetGraphicsRoot32BitConstants(0, 24, constants, 0);

    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    tl_activeCmdList->SetDescriptorHeaps(1, heaps);
    tl_activeCmdList->SetGraphicsRootDescriptorTable(1, textureSrv);

    tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    tl_activeCmdList->DrawInstanced(3, 1, 0, 0);
}

void D3D12Renderer::drawMeshTriangle(TextureRef texture,
                                      const glm::vec2 verts[3],
                                      const glm::vec2 uvs[3]) {
    const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveTextureHandle(texture);
    if (srv.ptr == 0) return;
    drawMeshTriangle(srv, verts, uvs);
}

// Show-thread overlay draw. Records on tl_activeCmdList, which is the show
// command list when called from OutputManager::renderToOutput between
// beginOutputFrame/endOutputFrame. The PSO targets the output swap chain
// backbuffer format (R8G8B8A8_UNORM) so the draw lands directly on the
// physical projector output — no compose-target indirection, no cross-
// thread compose-target race (ADR-0014).
//
// pointsXY: flat array of [x0,y0,x1,y1,...], length = numPoints * 2 floats.
// numPoints clamps to 16 (root-constant capacity).
void D3D12Renderer::drawCalibrationOverlay(const float* pointsXY, int numPoints,
                                            int activeIndex, bool precisionCursor) {
    // Lazily build the PSO on first use. PSO format is R8G8B8A8_UNORM
    // (output backbuffer), set in createCalibrationOverlayPSO above.
    if (!m_calibRootSignature || !m_calibPipelineState) {
        if (!createCalibrationOverlayPSO()) return;
    }
    if (!tl_activeCmdList) return;

    CalibrationConstants c{};
    c.numPts       = std::min(numPoints, 16);
    c.activeIdx    = activeIndex;
    c.checkerboard = precisionCursor ? 1 : 0;
    c.pts.fill(0.0f);
    for (int i = 0; i < c.numPts; ++i) {
        c.pts[i * 2]     = pointsXY[i * 2];
        c.pts[i * 2 + 1] = pointsXY[i * 2 + 1];
    }

    tl_activeCmdList->SetPipelineState(m_calibPipelineState.Get());
    tl_activeCmdList->SetGraphicsRootSignature(m_calibRootSignature.Get());
    tl_activeCmdList->SetGraphicsRoot32BitConstants(
        0,
        sizeof(CalibrationConstants) / 4,
        &c,
        0);
    tl_activeCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    tl_activeCmdList->DrawInstanced(3, 1, 0, 0);
}

// =============================================================================
// Stage 3D offscreen render target — editor thread only (ADR-0014)
// =============================================================================

bool D3D12Renderer::createStageRenderTarget(uint32_t width, uint32_t height) {
    auto* device = m_gpu->device();

    // Release any existing resources (resize path)
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (m_stageTarget.frameCBMapped[i] && m_stageTarget.frameCB[i]) {
            m_stageTarget.frameCB[i]->Unmap(0, nullptr);
            m_stageTarget.frameCBMapped[i] = nullptr;
        }
        m_stageTarget.frameCB[i].Reset();
        m_stageTarget.color[i].Reset();
        m_stageTarget.depth[i].Reset();
    }
    m_stageTarget.rtvHeap.Reset();
    m_stageTarget.dsvHeap.Reset();
    m_stageTarget.ready  = false;
    m_stageTarget.width  = 0;
    m_stageTarget.height = 0;

    // --- RTV descriptor heap (FRAME_COUNT RTVs, not shader-visible) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = FRAME_COUNT;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_stageTarget.rtvHeap));
        if (FAILED(hr)) {
            std::cerr << "[Stage3D] Failed to create stage RTV heap HRESULT 0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    // --- DSV descriptor heap (FRAME_COUNT DSVs, never shader-visible) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = FRAME_COUNT;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_stageTarget.dsvHeap));
        if (FAILED(hr)) {
            std::cerr << "[Stage3D] Failed to create stage DSV heap HRESULT 0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    const uint32_t rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const uint32_t dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        // --- Color resource: RGBA16F, ALLOW_RENDER_TARGET, initial state PIXEL_SHADER_RESOURCE ---
        {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = width;
            rd.Height           = height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            // Optimized clear must match the values used in ClearRenderTargetView.
            // Transparent black (0,0,0,0) so the ImGui window shows alpha.
            D3D12_CLEAR_VALUE cv{};
            cv.Format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
            cv.Color[0] = 0.0f; cv.Color[1] = 0.0f;
            cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

            HRESULT hr = device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &cv, IID_PPV_ARGS(&m_stageTarget.color[i]));
            if (FAILED(hr)) {
                std::cerr << "[Stage3D] Failed to create stage color[" << i << "] HRESULT 0x"
                          << std::hex << hr << std::dec << "\n";
                return false;
            }
            {
                wchar_t name[32];
                swprintf_s(name, L"StageColor%u", i);
                m_stageTarget.color[i]->SetName(name);
            }

            // RTV
            D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = m_stageTarget.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtvCpu.ptr += static_cast<SIZE_T>(i) * rtvSize;
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rtvDesc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = 0;
            device->CreateRenderTargetView(m_stageTarget.color[i].Get(), &rtvDesc, rtvCpu);

            // SRV in main heap
            const uint32_t heapSlot = DescriptorHeapLayout::stageTargetSlot(i);
            m_stageTarget.srvSlots[i] = heapSlot;
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = DescriptorHeapLayout::cpuHandle(
                m_imguiSrvHeap.Get(), heapSlot, m_srvDescriptorSize);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format                        = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels           = 1;
            srvDesc.Texture2D.MostDetailedMip     = 0;
            device->CreateShaderResourceView(m_stageTarget.color[i].Get(), &srvDesc, srvCpu);
        }

        // --- Depth resource: D32_FLOAT, ALLOW_DEPTH_STENCIL, initial state DEPTH_WRITE ---
        {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = width;
            rd.Height           = height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_D32_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            // Optimized clear must match ClearDepthStencilView args (depth=1.0, stencil=0).
            D3D12_CLEAR_VALUE cv{};
            cv.Format               = DXGI_FORMAT_D32_FLOAT;
            cv.DepthStencil.Depth   = 1.0f;
            cv.DepthStencil.Stencil = 0;

            HRESULT hr = device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &cv, IID_PPV_ARGS(&m_stageTarget.depth[i]));
            if (FAILED(hr)) {
                std::cerr << "[Stage3D] Failed to create stage depth[" << i << "] HRESULT 0x"
                          << std::hex << hr << std::dec << "\n";
                return false;
            }
            {
                wchar_t name[32];
                swprintf_s(name, L"StageDepth%u", i);
                m_stageTarget.depth[i]->SetName(name);
            }

            // DSV
            D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu = m_stageTarget.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            dsvCpu.ptr += static_cast<SIZE_T>(i) * dsvSize;
            device->CreateDepthStencilView(m_stageTarget.depth[i].Get(), nullptr, dsvCpu);
        }
    }

    // --- FrameCB: one persistently-mapped UPLOAD-heap buffer per frame slot ---
    // Indexed by m_currentBackBufferIndex so frame N+2's CPU write never aliases
    // frame N+1's buffer while the GPU is still executing N+1's draw commands.
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        // CBV requires 256-byte alignment; viewProj is 64 bytes.
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 256;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
            HRESULT hr = device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&m_stageTarget.frameCB[i]));
            if (FAILED(hr)) {
                std::cerr << "[Stage3D] Failed to create FrameCB[" << i << "] HRESULT 0x"
                          << std::hex << hr << std::dec << "\n";
                return false;
            }
            {
                wchar_t name[32];
                swprintf_s(name, L"StageFrameCB%u", i);
                m_stageTarget.frameCB[i]->SetName(name);
            }
            D3D12_RANGE readRange{0, 0};
            m_stageTarget.frameCB[i]->Map(0, &readRange, &m_stageTarget.frameCBMapped[i]);
        }
    }

    m_stageTarget.width  = width;
    m_stageTarget.height = height;
    m_stageTarget.ready  = true;

    std::cout << "[Stage3D] Stage render target created: " << width << "x" << height << "\n";
    return true;
}

bool D3D12Renderer::createStageRenderTargetStraight(uint32_t width, uint32_t height) {
    auto* device = m_gpu->device();

    // Release any existing resources (resize path)
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_stageTargetStraight.color[i].Reset();
    }
    m_stageTargetStraight.rtvHeap.Reset();
    m_stageTargetStraight.ready  = false;
    m_stageTargetStraight.width  = 0;
    m_stageTargetStraight.height = 0;

    // RTV heap (FRAME_COUNT RTVs, not shader-visible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = FRAME_COUNT;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&m_stageTargetStraight.rtvHeap));
        if (FAILED(hr)) {
            std::cerr << "[Stage3D-Straight] Failed to create RTV heap HRESULT 0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    const uint32_t rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // The de-premul PS unconditionally writes every pixel of the RT
        // (fullscreen triangle), so the clear color doesn't matter visually
        // — but D3D12 requires the optimized clear-value to match any
        // ClearRenderTargetView calls; we never call clear here, but the
        // value still has to be set for validation.
        D3D12_CLEAR_VALUE cv{};
        cv.Format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cv.Color[0] = 0.0f; cv.Color[1] = 0.0f;
        cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &cv, IID_PPV_ARGS(&m_stageTargetStraight.color[i]));
        if (FAILED(hr)) {
            std::cerr << "[Stage3D-Straight] Failed to create color[" << i << "] HRESULT 0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
        {
            wchar_t name[40];
            swprintf_s(name, L"StageStraightColor%u", i);
            m_stageTargetStraight.color[i]->SetName(name);
        }

        // RTV
        D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu =
            m_stageTargetStraight.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvCpu.ptr += static_cast<SIZE_T>(i) * rtvSize;
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtvDesc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        device->CreateRenderTargetView(m_stageTargetStraight.color[i].Get(), &rtvDesc, rtvCpu);

        // SRV in main heap
        const uint32_t heapSlot = DescriptorHeapLayout::stageTargetStraightSlot(i);
        m_stageTargetStraight.srvSlots[i] = heapSlot;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = DescriptorHeapLayout::cpuHandle(
            m_imguiSrvHeap.Get(), heapSlot, m_srvDescriptorSize);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        device->CreateShaderResourceView(m_stageTargetStraight.color[i].Get(), &srvDesc, srvCpu);
    }

    m_stageTargetStraight.width  = width;
    m_stageTargetStraight.height = height;
    m_stageTargetStraight.ready  = true;

    std::cout << "[Stage3D-Straight] RT created: " << width << "x" << height << "\n";
    return true;
}

void* D3D12Renderer::ensureStageTarget(uint32_t width, uint32_t height) {
    if (!m_initialized || !m_gpu || width == 0 || height == 0) return nullptr;

    if (!m_stageTarget.ready ||
        m_stageTarget.width  != width ||
        m_stageTarget.height != height) {
        waitForGpu();
        if (!createStageRenderTarget(width, height)) return nullptr;
    }

    // Sibling straight-alpha RT — fed by applyStageDePremul, sampled by
    // ImGui. Created / resized in lockstep with the premul RT.
    if (!m_stageTargetStraight.ready ||
        m_stageTargetStraight.width  != width ||
        m_stageTargetStraight.height != height) {
        if (!createStageRenderTargetStraight(width, height)) return nullptr;
    }

    // Lazily upload the shared default 16:9 screen quad once the GPU is ready.
    if (m_defaultScreenMeshSlot == UINT32_MAX && m_meshUploader) {
        MeshData defaultMesh = createDefaultScreenMesh();
        m_defaultScreenMeshSlot = uploadMeshImmediate(defaultMesh.vertices, defaultMesh.indices);
        if (m_defaultScreenMeshSlot == UINT32_MAX) {
            std::cerr << "[StageRenderer] Failed to upload default screen mesh — flat screens won't render.\n";
        }
    }

    // Return the straight-alpha sibling's SRV — ImGui composites this with
    // its standard SRC_ALPHA / INV_SRC_ALPHA pipeline. The premul RT stays
    // a renderer-internal accumulation buffer.
    const uint32_t slot = m_stageTargetStraight.srvSlots[m_currentBackBufferIndex];
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = DescriptorHeapLayout::gpuHandle(
        m_imguiSrvHeap.Get(), slot, m_srvDescriptorSize);
    return reinterpret_cast<void*>(gpu.ptr);
}

void D3D12Renderer::beginStageTarget() {
    if (!m_stageTarget.ready) return;

    const uint32_t fi = m_currentBackBufferIndex;
    auto* cmdList = tl_activeCmdList;

    // Transition color: PIXEL_SHADER_RESOURCE → RENDER_TARGET
    D3D12_RESOURCE_BARRIER colorBarrier{};
    colorBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    colorBarrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    colorBarrier.Transition.pResource   = m_stageTarget.color[fi].Get();
    colorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    colorBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    colorBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &colorBarrier);

    const uint32_t rtvSize = m_gpu->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const uint32_t dsvSize = m_gpu->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = m_stageTarget.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvCpu.ptr += static_cast<SIZE_T>(fi) * rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu = m_stageTarget.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsvCpu.ptr += static_cast<SIZE_T>(fi) * dsvSize;

    // Set viewport + scissor to stage target dimensions
    D3D12_VIEWPORT vp{};
    vp.Width    = static_cast<float>(m_stageTarget.width);
    vp.Height   = static_cast<float>(m_stageTarget.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor{};
    scissor.right  = static_cast<LONG>(m_stageTarget.width);
    scissor.bottom = static_cast<LONG>(m_stageTarget.height);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &scissor);

    // Bind RTV + DSV
    cmdList->OMSetRenderTargets(1, &rtvCpu, FALSE, &dsvCpu);

    // Clear color (transparent black) and depth (1.0)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearRenderTargetView(rtvCpu, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(dsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void D3D12Renderer::endStageTarget() {
    if (!m_stageTarget.ready) return;

    const uint32_t fi = m_currentBackBufferIndex;
    auto* cmdList = tl_activeCmdList;

    // Transition color: RENDER_TARGET → PIXEL_SHADER_RESOURCE for ImGui sampling
    D3D12_RESOURCE_BARRIER colorBarrier{};
    colorBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    colorBarrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    colorBarrier.Transition.pResource   = m_stageTarget.color[fi].Get();
    colorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    colorBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    colorBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &colorBarrier);

    // Restore viewport + scissor to back-buffer size for subsequent ImGui rendering
    D3D12_VIEWPORT vp{};
    vp.Width    = static_cast<float>(m_width);
    vp.Height   = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor{};
    scissor.right  = static_cast<LONG>(m_width);
    scissor.bottom = static_cast<LONG>(m_height);
    tl_activeCmdList->RSSetViewports(1, &vp);
    tl_activeCmdList->RSSetScissorRects(1, &scissor);

    // Rebind the editor back-buffer RT — beginStageTarget bound the stage RT+DSV;
    // ImGui's subsequent draws would otherwise render into a target we just
    // transitioned to PIXEL_SHADER_RESOURCE (device removal).
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_currentBackBufferIndex * m_rtvDescriptorSize;
    tl_activeCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
}

// =============================================================================
// Stage mesh PSO — depth-buffered, cull-none, RGBA16F + D32_FLOAT
// Root signature:
//   [0] CBV root descriptor  b0 = FrameCB  (viewProj, 64 bytes)
//   [1] 32-bit constants     b1 = DrawCB   (model 16f + tint 4f + renderMode 1u + pad 3u = 24 DWORDs)
//   [2] descriptor table     t0 = mesh texture SRV
//   static sampler           s0 = linear/clamp
// =============================================================================

bool D3D12Renderer::createStageMeshPSO() {
    // --- Root signature ---
    D3D12_ROOT_PARAMETER rootParams[3] = {};

    // [0] CBV root descriptor — b0 (FrameCB: viewProj)
    rootParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister           = 0; // b0
    rootParams[0].Descriptor.RegisterSpace            = 0;
    rootParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

    // [1] 32-bit constants — b1 (DrawCB: model 16f + tint 4f + renderMode 1u + _pad 3u = 24 DWORDs)
    rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[1].Constants.ShaderRegister            = 1; // b1
    rootParams[1].Constants.RegisterSpace             = 0;
    rootParams[1].Constants.Num32BitValues            = 24;
    rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Descriptor table — t0 (mesh texture SRV)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType                               = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                          = 1;
    srvRange.BaseShaderRegister                      = 0; // t0
    srvRange.RegisterSpace                           = 0;
    srvRange.OffsetInDescriptorsFromTableStart       = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rootParams[2].ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges  = &srvRange;
    rootParams[2].ShaderVisibility                   = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0; // s0
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = 3;
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;
    // IA layout flag required for vertex + index buffer binding.
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS        |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS      |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "[StageMesh] root sig: "
                           << static_cast<const char*>(err->GetBufferPointer()) << "\n";
        return false;
    }
    hr = m_gpu->device()->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                              IID_PPV_ARGS(&m_stageMeshRootSignature));
    if (FAILED(hr)) {
        std::cerr << "[StageMesh] CreateRootSignature hr=0x" << std::hex << hr << "\n";
        return false;
    }

    // --- Shaders ---
    ComPtr<ID3DBlob> vs, ps;
    if (loadCompiledShader(L"shaders/stage_mesh_vs.cso", &vs) != Result::Success) {
        std::cerr << "[StageMesh] missing stage_mesh_vs.cso\n"; return false;
    }
    if (loadCompiledShader(L"shaders/stage_mesh_ps.cso", &ps) != Result::Success) {
        std::cerr << "[StageMesh] missing stage_mesh_ps.cso\n"; return false;
    }

    // --- Input layout matching MeshVertex: pos float3 @ 0, uv float2 @ 12, nrm float3 @ 20 ---
    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // --- PSO ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_stageMeshRootSignature.Get();
    psoDesc.VS                    = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS                    = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.InputLayout           = { inputElements, _countof(inputElements) };

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE; // mesh exports have inconsistent winding
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    psoDesc.SampleMask                       = UINT_MAX;
    psoDesc.PrimitiveTopologyType            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets                 = 1;
    psoDesc.RTVFormats[0]                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.DSVFormat                        = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count                 = 1;

    hr = m_gpu->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_stageMeshPipelineState));
    if (FAILED(hr)) {
        std::cerr << "[StageMesh] CreateGraphicsPipelineState hr=0x" << std::hex << hr << "\n";
        return false;
    }

    // --- Transparent variant: premultiplied "over" blend, no depth write ---
    // Used for back-to-front sorted transparent meshes so multiple scrims
    // (or a scrim in front of an opaque set piece) blend correctly within
    // the stage RT. Depth-test stays LESS_EQUAL so a transparent mesh
    // hidden behind an opaque one depth-fails; depth-write off so multiple
    // transparent meshes don't occlude each other.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC tDesc = psoDesc;
        auto& rt = tDesc.BlendState.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D12_BLEND_ONE;            // shader outputs premul RGB
        rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        tDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        hr = m_gpu->device()->CreateGraphicsPipelineState(
            &tDesc, IID_PPV_ARGS(&m_stageMeshPipelineStateTransparent));
        if (FAILED(hr)) {
            std::cerr << "[StageMesh] CreateGraphicsPipelineState (transparent) hr=0x"
                      << std::hex << hr << "\n";
            return false;
        }
    }

    std::cout << "[StageMesh] PSOs created (opaque + transparent)\n";
    return true;
}

// =============================================================================
// Stage de-premultiply pass — fullscreen-triangle pass that samples the
// premul stage RT and writes straight-alpha RGBA into the sibling straight
// RT (m_stageTargetStraight). ImGui samples the straight RT so its standard
// SRC_ALPHA / INV_SRC_ALPHA blend produces the right composite.
// =============================================================================

bool D3D12Renderer::createStageDePremulPSO() {
    auto* device = m_gpu->device();

    // --- Root signature: one SRV table (t0) + static sampler (s0). No CBV. ---
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0; // t0
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParam.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0; // s0
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = 1;
    rsDesc.pParameters       = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS   |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::cerr << "[StageDePremul] root sig: "
                           << static_cast<const char*>(err->GetBufferPointer()) << "\n";
        return false;
    }
    hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                     IID_PPV_ARGS(&m_stageDePremulRootSignature));
    if (FAILED(hr)) {
        std::cerr << "[StageDePremul] CreateRootSignature hr=0x" << std::hex << hr << "\n";
        return false;
    }

    // --- Shaders ---
    ComPtr<ID3DBlob> vs, ps;
    if (loadCompiledShader(L"shaders/stage_depremul_vs.cso", &vs) != Result::Success) {
        std::cerr << "[StageDePremul] missing stage_depremul_vs.cso\n"; return false;
    }
    if (loadCompiledShader(L"shaders/stage_depremul_ps.cso", &ps) != Result::Success) {
        std::cerr << "[StageDePremul] missing stage_depremul_ps.cso\n"; return false;
    }

    // --- PSO: fullscreen triangle, no vertex buffer, no depth, no blend ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_stageDePremulRootSignature.Get();
    psoDesc.VS                    = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS                    = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask                = UINT_MAX;
    psoDesc.PrimitiveTopologyType     = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets          = 1;
    psoDesc.RTVFormats[0]             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.SampleDesc.Count          = 1;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_stageDePremulPipelineState));
    if (FAILED(hr)) {
        std::cerr << "[StageDePremul] CreateGraphicsPipelineState hr=0x" << std::hex << hr << "\n";
        return false;
    }

    std::cout << "[StageDePremul] PSO created\n";
    return true;
}

void D3D12Renderer::setStageCamera(const glm::mat4& viewProj) {
    const uint32_t fi = m_currentBackBufferIndex;
    if (!m_stageTarget.frameCBMapped[fi]) return;
    // glm::mat4 is column-major, matching HLSL float4x4 column-major layout.
    std::memcpy(m_stageTarget.frameCBMapped[fi], &viewProj[0][0], sizeof(float) * 16);
}

void D3D12Renderer::drawStageMesh(uint32_t meshSlot,
                                   const glm::mat4& model,
                                   const glm::vec4& tint,
                                   uint32_t renderMode,
                                   uint32_t textureSrvSlot,
                                   bool     transparent) {
    if (!m_initialized || !m_stageTarget.ready) return;

    if (!m_stageMeshRootSignature || !m_stageMeshPipelineState
                                  || !m_stageMeshPipelineStateTransparent) {
        if (!createStageMeshPSO()) return;
    }

    MeshUploader::BindHandle handle{};
    if (!m_meshUploader || !m_meshUploader->getBindHandle(meshSlot, handle)) return;

    // Pack DrawCB: model (16f) + tint (4f) + renderMode (1u) + _pad (3u) = 24 DWORDs
    struct DrawCB {
        float    model[16];
        float    tint[4];
        uint32_t renderMode;
        uint32_t _pad[3];
    };
    static_assert(sizeof(DrawCB) == 24 * 4, "DrawCB must be 24 DWORDs");

    DrawCB cb{};
    std::memcpy(cb.model, &model[0][0], sizeof(float) * 16);
    cb.tint[0]    = tint.x; cb.tint[1] = tint.y;
    cb.tint[2]    = tint.z; cb.tint[3] = tint.w;
    cb.renderMode = renderMode;

    auto* cmdList = tl_activeCmdList;

    cmdList->SetGraphicsRootSignature(m_stageMeshRootSignature.Get());
    cmdList->SetPipelineState(transparent
        ? m_stageMeshPipelineStateTransparent.Get()
        : m_stageMeshPipelineState.Get());

    // [0] CBV root descriptor — FrameCB (viewProj), per-frame slot avoids write-while-read
    cmdList->SetGraphicsRootConstantBufferView(0, m_stageTarget.frameCB[m_currentBackBufferIndex]->GetGPUVirtualAddress());

    // [1] DrawCB root constants
    cmdList->SetGraphicsRoot32BitConstants(1, 24, &cb, 0);

    // [2] Texture SRV descriptor table
    if (tl_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        tl_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }
    D3D12_GPU_DESCRIPTOR_HANDLE texGpu = DescriptorHeapLayout::gpuHandle(
        m_imguiSrvHeap.Get(), textureSrvSlot, m_srvDescriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(2, texGpu);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &handle.vbv);
    cmdList->IASetIndexBuffer(&handle.ibv);
    cmdList->DrawIndexedInstanced(handle.indexCount, 1, 0, 0, 0);
}

void D3D12Renderer::applyStageDePremul() {
    if (!m_initialized) return;
    if (!m_stageTarget.ready || !m_stageTargetStraight.ready) return;

    // Lazy PSO creation — first stage frame after init.
    if (!m_stageDePremulRootSignature || !m_stageDePremulPipelineState) {
        if (!createStageDePremulPSO()) return;
    }

    const uint32_t fi = m_currentBackBufferIndex;
    auto* cmdList = tl_activeCmdList;
    if (!cmdList) return;

    // endStageTarget() already transitioned the premul color RT from
    // RENDER_TARGET → PIXEL_SHADER_RESOURCE for sampling — no barrier
    // needed here for the source.
    //
    // Move the straight sibling to RENDER_TARGET so we can write to it.
    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource   = m_stageTargetStraight.color[fi].Get();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toRT);

    const uint32_t rtvSize =
        m_gpu->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu =
        m_stageTargetStraight.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvCpu.ptr += static_cast<SIZE_T>(fi) * rtvSize;

    cmdList->OMSetRenderTargets(1, &rtvCpu, FALSE, nullptr);

    // Viewport + scissor = straight RT dimensions (matches premul RT).
    D3D12_VIEWPORT vp{};
    vp.Width    = static_cast<float>(m_stageTargetStraight.width);
    vp.Height   = static_cast<float>(m_stageTargetStraight.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT scissor{};
    scissor.right  = static_cast<LONG>(m_stageTargetStraight.width);
    scissor.bottom = static_cast<LONG>(m_stageTargetStraight.height);
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &scissor);

    // Bind heap + PSO + root sig + premul SRV table.
    if (tl_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        tl_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }
    cmdList->SetGraphicsRootSignature(m_stageDePremulRootSignature.Get());
    cmdList->SetPipelineState(m_stageDePremulPipelineState.Get());

    const uint32_t premulSlot = DescriptorHeapLayout::stageTargetSlot(fi);
    D3D12_GPU_DESCRIPTOR_HANDLE premulGpu = DescriptorHeapLayout::gpuHandle(
        m_imguiSrvHeap.Get(), premulSlot, m_srvDescriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(0, premulGpu);

    // Fullscreen triangle — no vertex / index buffer.
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->DrawInstanced(3, 1, 0, 0);

    // Straight RT back to PIXEL_SHADER_RESOURCE so ImGui can sample it.
    D3D12_RESOURCE_BARRIER toSRV{};
    toSRV.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSRV.Transition.pResource   = m_stageTargetStraight.color[fi].Get();
    toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toSRV.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSRV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toSRV);

    // Restore the editor back-buffer RTV (same restore endStageTarget does,
    // so ImGui's next draw command targets the right place).
    D3D12_CPU_DESCRIPTOR_HANDLE backRtv =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    backRtv.ptr += m_currentBackBufferIndex * m_rtvDescriptorSize;
    cmdList->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
}

uint32_t D3D12Renderer::uploadMeshImmediate(const std::vector<MeshVertex>& vertices,
                                             const std::vector<uint32_t>& indices) {
    if (!m_meshUploader || vertices.empty() || indices.empty()) return MeshUploader::INVALID_SLOT;

    // One-shot copy-queue submission using the editor-thread-dedicated copy
    // command list and allocators (separate from the show thread's copy
    // resources so there is no concurrent command list recording).
    const uint32_t ci = m_currentBackBufferIndex;
    HRESULT hr = m_editorCopyCommandAllocators[ci]->Reset();
    if (FAILED(hr)) {
        std::cerr << "[MeshUploader] editor copy allocator Reset failed: " << std::hex << hr << std::endl;
        return MeshUploader::INVALID_SLOT;
    }
    hr = m_editorCopyCommandList->Reset(m_editorCopyCommandAllocators[ci].Get(), nullptr);
    if (FAILED(hr)) {
        std::cerr << "[MeshUploader] editor copy command list Reset failed: " << std::hex << hr << std::endl;
        return MeshUploader::INVALID_SLOT;
    }

    uint32_t slot = m_meshUploader->uploadMesh(m_editorCopyCommandList.Get(), vertices, indices);
    if (slot == MeshUploader::INVALID_SLOT) {
        m_editorCopyCommandList->Close();
        return MeshUploader::INVALID_SLOT;
    }

    hr = m_editorCopyCommandList->Close();
    if (FAILED(hr)) {
        std::cerr << "[MeshUploader] editor copy command list Close failed: " << std::hex << hr << std::endl;
        return MeshUploader::INVALID_SLOT;
    }

    ID3D12CommandList* copyLists[] = { m_editorCopyCommandList.Get() };
    m_gpu->copyQueue()->ExecuteCommandLists(1, copyLists);
    const uint64_t copyValue = ++m_uploadFenceValue;
    m_gpu->copyQueue()->Signal(m_uploadFence.Get(), copyValue);
    // Cross-queue wait: direct queue must not read the vertex/index buffers
    // until the copy queue has finished writing them.
    m_gpu->commandQueue()->Wait(m_uploadFence.Get(), copyValue);

    std::cout << "[MeshUploader] uploaded mesh to slot " << slot
              << " (" << vertices.size() << " verts, " << indices.size() << " indices)" << std::endl;
    return slot;
}

uint64_t D3D12Renderer::getMeshUploadCount() const {
    return m_meshUploader ? m_meshUploader->uploadCount() : 0;
}

void D3D12Renderer::scheduleMeshSlotFree(uint32_t slot) {
    if (!m_meshUploader || slot == MeshUploader::INVALID_SLOT) return;
    // Key on the editor fence value that will be signaled at the next
    // endEditorFrame. m_editorRingSlot (the monotonic ring slot, NOT the
    // swap-chain back-buffer index) is the entry in m_editorFenceValues being
    // built this frame — moveToNextFrame signals and advances that same slot,
    // so the deferred free must read the same key or it can free a GPU-live
    // mesh buffer (the use-after-free class this investigation fixed).
    //
    // Lift the floor to GetCompletedValue() before +1. A waitForGpu() drain
    // mid-session Signals the editor fence to (completed + 1) but deliberately
    // does NOT update m_editorFenceValues[] (those stay owned by
    // moveToNextFrame). So after a drain, m_editorFenceValues[slot] can lag far
    // below the fence's completed value — and m_editorFenceValues[slot] + 1
    // would then be <= GetCompletedValue(), so MeshUploader::onFrameBegin
    // (reaps when fenceValue <= completed) frees the slot on the very next
    // frame while the frame that references this mesh is still in flight: a
    // GPU-live free (page-fault / DEVICE_HUNG class). std::max with the
    // completed value pins the target to the value the current frame will
    // actually signal via moveToNextFrame's own std::max, so the reap can only
    // fire after that signal lands.
    const uint64_t fenceValue =
        std::max(m_editorFenceValues[m_editorRingSlot],
                 m_editorFence->GetCompletedValue()) + 1;
    m_meshUploader->scheduleSlotFree(slot, fenceValue);
}

} // namespace entity
