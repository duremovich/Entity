#pragma once

/**
 * D3D12Renderer - Direct3D 12 rendering backend
 *
 * Handles D3D12 device initialization, swap chain management, and rendering.
 * This is a minimal implementation to get something on screen.
 */

#include "entity/core/Types.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/media/ObjLoader.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/render/DescriptorHeapLayout.hpp"
#include "entity/render/EditorReadGate.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/render/RuntimeShaderCompiler.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations
struct GLFWwindow;

namespace entity {

class OcioManager;
class OcioGpuProcessor;

using Microsoft::WRL::ComPtr;

// D3D12-backed implementation of IRenderer. The class retains some
// D3D12-typed public methods (e.g. drawTexturedQuad taking
// D3D12_GPU_DESCRIPTOR_HANDLE) during the Phase B migration — those are the
// non-virtual legacy overloads. The virtual IRenderer methods use abstract
// types (TextureRef, glm::*) and delegate to the legacy methods internally.
// Once all external callers migrate to the IRenderer API (Phase B task 18),
// the legacy D3D12-typed methods will be removed.
class D3D12Renderer : public IRenderer {
public:
    D3D12Renderer();
    ~D3D12Renderer() override;

    // ========================================================================
    // IRenderer — backend-agnostic API
    // ========================================================================

    Result initialize(GLFWwindow* window, uint32_t width, uint32_t height) override;
    void   shutdown() override;
    Result resize(uint32_t width, uint32_t height) override;
    bool    isInitialized() const override        { return m_initialized; }
    bool    isDeviceLost() const override         { return m_deviceLost.load(std::memory_order_acquire); }
    int32_t getDeviceLostReason() const override  { return static_cast<int32_t>(m_deviceLostReason.load(std::memory_order_acquire)); }

    // Test seams: set m_deviceLost and exercise waitForGpu() without a real device.
    // Used by WaitForGpuEarlyOutTests to verify waitForGpu() exits immediately.
    // Store the reason (release) before latching m_deviceLost so the shutdown
    // gate and getDeviceLostReason() see a CONFIRMED removal — the default
    // DXGI_ERROR_DEVICE_HUNG (!= S_OK) makes the shutdown gate skip the drain.
    void setDeviceLostForTesting(HRESULT reason = DXGI_ERROR_DEVICE_HUNG) {
        m_deviceLostReason.store(reason, std::memory_order_release);
        m_deviceLost.store(true, std::memory_order_release);
    }
    void waitForGpuForTesting()     { waitForGpu(); }

    // Headless/CI device-lost policy (issue #69). When enabled,
    // handleDeviceLost() calls std::_Exit(kDeviceLostExitCode) right after the
    // crash record is written, instead of returning into the normal shutdown
    // path. On a removed/hung device that path blocks inside the driver's COM
    // release, ctest SIGKILLs the process at its timeout, and the killed
    // process (still holding the device + swapchain) cascades the wedge into
    // the next test's startup. Never enabled in interactive mode — the GUI's
    // device-lost flow (autosave + --device-lost-recovery relaunch in
    // apps/editor/main.cpp) requires the normal shutdown to complete.
    void setExitProcessOnDeviceLost(bool enable) {
        m_exitProcessOnDeviceLost.store(enable, std::memory_order_relaxed);
    }
    // Distinct from EXIT_FAILURE(1) (script command failures) so a ctest log
    // shows device-loss as its own failure class.
    static constexpr int kDeviceLostExitCode = 4;

    void     clear(float r, float g, float b, float a) override;
    uint32_t getCurrentBackBufferIndex() const override { return m_currentBackBufferIndex; }
    void     beginImGuiFrame() override;
    void     endImGuiFrame() override;

    // Stage 1 A/B-list split. Show-side owns copy uploads + compositor +
    // output draws + output Present. Editor-side owns back-buffer setup,
    // ImGui recording, and editor swap-chain Present.
    void beginShowFrame() override;
    void endShowFrame() override;
    void beginEditorFrame() override;
    void endEditorFrame() override;

    uint32_t   allocateVideoTextureSlot() override;
    void       scheduleVideoTextureSlotFree(uint32_t slot) override;
    uint32_t   getVideoTextureSlotsAllocated() const override;
    uint32_t   videoTextureSlotGeneration(uint32_t slot) const override;
    uint64_t   videoTextureStaleGenerationSkips() const override;
    uint64_t   videoTextureStaleGenerationDrawSkips() const override;
    bool       prepareVideoTextureSlot(uint32_t slot,
                                       uint32_t width,
                                       uint32_t height,
                                       TextureFormat format =
                                           TextureFormat::RGBA8_UNORM) override;
    bool       uploadVideoFrameToSlot(uint32_t slot,
                                      const uint8_t* rgba,
                                      uint32_t width,
                                      uint32_t height) override;
    bool       uploadVideoFrameToSlot(uint32_t slot,
                                      const uint8_t* data,
                                      uint32_t width,
                                      uint32_t height,
                                      TextureFormat format,
                                      uint32_t expectedGeneration = UINT32_MAX) override;
    bool       uploadVideoFrameToSlotImmediate(uint32_t slot,
                                               const uint8_t* rgba,
                                               uint32_t width,
                                               uint32_t height) override;
    bool       copyUploadBufferToVideoTextureSlot(
                   uint32_t slot,
                   UploadHeapBuffer* buf,
                   uint32_t width,
                   uint32_t height,
                   TextureFormat format,
                   uint32_t expectedGeneration = UINT32_MAX) override;

    // Phase 5 — wire the UploadHeapBufferPool so copyUploadBufferToVideoTextureSlot
    // can call recordCopyDone on buffers after recording CopyTextureRegion.
    // Called by Renderer::initialize after both the D3D12 backend and the pool
    // are constructed. The pool outlives the renderer (Renderer owns both;
    // m_uploadHeapPool destructs before m_d3d12Renderer in reverse-declaration
    // order — safe because D3D12Renderer only holds a weak raw pointer here).
    void       setUploadHeapPool(class UploadHeapBufferPool* pool) {
        m_uploadHeapPool = pool;
    }
    TextureRef getVideoTexture(uint32_t slot) const override;

    uint32_t   createComposeTarget(uint32_t width, uint32_t height) override;
    bool       resizeComposeTarget(uint32_t slot, uint32_t width, uint32_t height) override;
    void       beginComposeTarget(uint32_t slot = 0) override;
    void       endComposeTarget() override;
    TextureRef getComposeTargetTexture(uint32_t slot = 0) const override;
    void*      getComposeTargetTextureID(uint32_t slot = 0) const override;
    uint32_t   getComposeTargetWidth(uint32_t slot = 0) const override;
    uint32_t   getComposeTargetHeight(uint32_t slot = 0) const override;
    bool       isComposeTargetReady(uint32_t slot = 0) const override;
    void*      getVideoTextureIDForSlot(uint32_t slot) const override;

    void drawColoredQuad(const glm::mat4& transform,
                          const glm::vec4& color,
                          float opacity) override;

    void drawTexturedQuad(TextureRef texture,
                           const glm::mat4& transform,
                           float opacity,
                           BlendMode blendMode = BlendMode::Normal,
                           TextureColorSpace colorSpace = TextureColorSpace::Linear,
                           const std::string& ocioColorSpace = std::string(),
                           const glm::vec4& uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)) override;

    void drawOutputSurface(TextureRef texture,
                             const glm::vec2 corners[4],
                             const glm::vec2 sourceUVs[4],
                             const glm::vec4& softEdges,
                             float brightness,
                             float gamma,
                             float opacity) override;

    uint32_t createOutputWindow(const char* title,
                                 int32_t x, int32_t y,
                                 uint32_t width, uint32_t height) override;
    void     destroyOutputWindow(uint32_t outputSlot) override;
    void     resizeOutputWindow(uint32_t outputSlot,
                                 uint32_t width, uint32_t height) override;
    uint32_t getOutputWindowWidth(uint32_t outputSlot) const override;
    uint32_t getOutputWindowHeight(uint32_t outputSlot) const override;
    void     beginOutputFrame(uint32_t outputSlot) override;
    void     clearOutputFrame(uint32_t outputSlot,
                               float r, float g, float b, float a) override;
    void     endOutputFrame(uint32_t outputSlot) override;

    bool captureComposeTargetToPNG(const std::string& filepath, uint32_t slot = 0) override;
    bool captureBackBufferToPNG(const std::string& filepath) override;
    bool readComposeTargetPixels(uint32_t slot,
                                  uint32_t& outWidth,
                                  uint32_t& outHeight,
                                  std::vector<uint8_t>& outPixels) override;

    // -----------------------------------------------------------------------
    // Projector calibration overlay
    //
    // Show-thread draw: records on the currently-bound output RTV
    // (R8G8B8A8_UNORM) between beginOutputFrame/endOutputFrame. Draws crosshair
    // markers at the given projector-UV positions; when precisionCursor is
    // true, draws a quadrant checkerboard centered on the active crosshair
    // for sub-pixel placement. All state travels via parameters — no renderer-
    // member state shared across threads.
    //
    // pointsXY: flat array [x0,y0,x1,y1,...] of length numPoints*2 (max 32 floats).
    // -----------------------------------------------------------------------
    void drawCalibrationOverlay(const float* pointsXY, int numPoints,
                                int activeIndex, bool precisionCursor) override;

    // -----------------------------------------------------------------------
    // Stage 3D offscreen render target — editor thread only (ADR-0014).
    //
    // ensureStageTarget lazily creates or resizes the double-buffered color +
    // depth resources, DSV heap, RTVs, and SRVs. Returns an ImTextureID
    // (GPU descriptor handle ptr cast to void*) suitable for ImGui::Image.
    // beginStageTarget / endStageTarget bracket a single editor draw pass.
    // setStageCamera uploads viewProj into the per-frame FrameCB; call once
    // per pass before any drawStageMesh calls.
    // -----------------------------------------------------------------------
    void* ensureStageTarget(uint32_t width, uint32_t height);
    void  beginStageTarget();
    void  endStageTarget();
    void  setStageCamera(const glm::mat4& viewProj);
    // `transparent=true` selects the no-depth-write premul-over PSO used
    // for back-to-front sorted transparent meshes (stage-view scrims).
    // Default false uses the opaque depth-writing PSO.
    void  drawStageMesh(uint32_t meshSlot,
                        const glm::mat4& model,
                        const glm::vec4& tint,
                        uint32_t renderMode,
                        uint32_t textureSrvSlot,
                        bool     transparent = false);
    // Fullscreen pass that converts the premul stage RT into the straight-
    // alpha sibling RT. Called by Stage3DRenderer::renderMeshes between
    // endStageTarget() and handing the SRV to ImGui. Without this, ImGui's
    // straight-alpha composite would double-multiply alpha and darken
    // transparent meshes against the editor background.
    void  applyStageDePremul();

    // -----------------------------------------------------------------------
    // Mesh lifecycle — editor thread only (ADR-0014).
    //
    // uploadMeshImmediate: opens a one-shot copy-queue submission, calls
    // MeshUploader::uploadMesh, executes + signals the upload fence, inserts
    // a cross-queue Wait so the direct queue doesn't read the buffers until
    // the copy completes. Returns the slot index, or UINT32_MAX on failure.
    //
    // scheduleMeshSlotFree: defers a slot release until the editor fence
    // advances past the value currently at endEditorFrame. Safe to call from
    // on_destroy<Model> callbacks on the editor thread.
    // -----------------------------------------------------------------------
    uint32_t uploadMeshImmediate(const std::vector<MeshVertex>& vertices,
                                  const std::vector<uint32_t>& indices);
    void     scheduleMeshSlotFree(uint32_t slot);

    // Returns the MeshUploader slot for the shared default 16:9 screen quad.
    // Uploaded once lazily on first ensureStageTarget call; UINT32_MAX until then.
    uint32_t getDefaultScreenMeshSlot() const { return m_defaultScreenMeshSlot; }

    // Total successful mesh uploads since construction. Used by integration tests
    // to verify upload pacing; load-bearing for mesh_upload_paces.json.
    uint64_t getMeshUploadCount() const;

    // Heap slot of the *stable* (last fully-rendered) sub-resource for a
    // compose target. Compose targets are triple-buffered; the show thread
    // rotates through writes. Reading from the base slot races against the
    // show thread's RT writes — use this for editor-thread sampling instead.
    // Returns UINT32_MAX if the slot is invalid or no frame is stable yet.
    uint32_t getComposeTargetStableSrvSlot(uint32_t slot) const;

    // -----------------------------------------------------------------------
    // Mesh-triangle rendering for projector output. Renders one triangle
    // (3 NDC verts + 3 UVs) of a textured mesh with proper hardware
    // barycentric UV interpolation — no bilinear approximation artifacts.
    // Must be called between beginOutputFrame / endOutputFrame.
    // -----------------------------------------------------------------------
    // TextureRef variant — IRenderer override.
    void     drawMeshTriangle(TextureRef texture,
                              const glm::vec2 verts[3],
                              const glm::vec2 uvs[3]) override;
    // D3D12-native variant.
    void     drawMeshTriangle(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                              const glm::vec2 verts[3],
                              const glm::vec2 uvs[3]);

    // Per-layer effect pass (issue #54). See IRenderer comment for contract.
    void     drawEffectPass(TextureRef input,
                            std::uint32_t kindIdHash,
                            const std::uint8_t* paramBlob,
                            std::size_t paramBlobSize,
                            std::uint32_t viewportWidth,
                            std::uint32_t viewportHeight) override;

    // Set the EffectKindRegistry pointer used to resolve kindIdHash → PS .cso
    // path. The registry is owned by Engine and outlives the renderer. The
    // pointer is read-only — engine effects register at startup and never
    // mutate; user effects mutate only on the editor thread (Phase 6).
    void     setEffectKindRegistry(const effects::EffectKindRegistry* reg) {
        m_effectKindRegistry = reg;
    }

    // Expose the runtime DXC compiler so callers outside the renderer
    // (notably Engine, for EffectKindRegistry::scanUserEffects) can
    // compile HLSL through the same DLL load + cache. Read-only access;
    // the compiler is thread-safe.
    RuntimeShaderCompiler& getRuntimeShaderCompiler() { return m_runtimeCompiler; }

    /**
     * Phase C.12 #5 — bind the OcioManager that drives input + display
     * transform PSO selection. Pass nullptr to disable OCIO and fall back
     * to the offline-compiled PSOs (sRGB-stub capture, no input transform,
     * legacy gamma-only mapping surface).
     *
     * Engine calls this once after both the renderer and OcioManager are
     * initialized. Subtask 7's "OCIO config reload" path will re-call it.
     *
     * Eagerly compiles the default input + display + capture PSOs so the
     * first frame doesn't pay a runtime DXC cost.
     */
    void setOcioManager(OcioManager* mgr);

    // ========================================================================
    // Legacy D3D12-typed API (kept during Phase B migration; callers in
    // Engine.cpp, CompositorSystem, OutputManager still use these. Task 18
    // migrates them to the TextureRef/glm overloads above, then these go away)
    // ========================================================================

    /**
     * Draw a colored quad with transform and opacity. Legacy D3D12-typed overload.
     */
    void drawColoredQuad(const DirectX::XMMATRIX& transform,
                         const DirectX::XMFLOAT4& color,
                         float opacity);

    /**
     * Upload RGBA video frame data to GPU texture.
     * Creates texture on first call, updates on subsequent calls.
     *
     * @param rgbaData Pointer to RGBA pixel data (4 bytes per pixel)
     * @param width Width of the frame in pixels
     * @param height Height of the frame in pixels
     * @return ImTextureID for use with ImGui::Image, or nullptr on failure
     */
    void* uploadVideoFrame(const uint8_t* rgbaData, uint32_t width, uint32_t height);

    /**
     * Get the current video texture ID for ImGui.
     * Returns nullptr if no video frame has been uploaded.
     */
    void* getVideoTextureID() const;

    /**
     * Get video texture dimensions.
     */
    uint32_t getVideoTextureWidth() const { return m_videoTextureWidth; }
    uint32_t getVideoTextureHeight() const { return m_videoTextureHeight; }

    /** Upload frame data to a slot, returning the D3D12 SRV handle. Internal. */
    bool uploadVideoFrameToSlot(uint32_t slot,
                                const uint8_t* data,
                                uint32_t width, uint32_t height,
                                D3D12_GPU_DESCRIPTOR_HANDLE* outSrvHandle,
                                TextureFormat format = TextureFormat::RGBA8_UNORM,
                                uint32_t expectedGeneration = UINT32_MAX);

    /** Draw a textured quad with D3D12 SRV + XMMATRIX. Legacy. */
    void drawTexturedQuad(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                          const DirectX::XMMATRIX& transform,
                          float opacity,
                          BlendMode blendMode = BlendMode::Normal,
                          TextureColorSpace colorSpace = TextureColorSpace::Linear,
                          const std::string& ocioColorSpace = std::string(),
                          const DirectX::XMFLOAT4& uvRect = DirectX::XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f));

    /** Draw a mapping surface with D3D12 SRV + XMFLOAT args. Legacy. */
    void drawOutputSurface(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                            const DirectX::XMFLOAT2 corners[4],
                            const DirectX::XMFLOAT2 sourceUVs[4],
                            const DirectX::XMFLOAT4& softEdges,
                            float brightness,
                            float gamma,
                            float opacity);

    /** Legacy: compose target GPU handle for the D3D12-typed draw methods. */
    D3D12_GPU_DESCRIPTOR_HANDLE getComposeTargetSrvHandle(uint32_t slot = 0) const;

    /** D3D12-specific escape hatch for external code that needs the device.
     *  Going away in Phase B once callers use IRenderer. */
    ID3D12Device* getDevice() const;  // Defined in .cpp so the header doesn't need D3D12Device.hpp.

    // Accessors for the direct command queue and show fence.
    // Used by Engine to wire UploadHeapBufferPool (Phase 5+) without
    // creating a circular dependency on D3D12Renderer internals.
    // Both return nullptr when the renderer is uninitialized.
    ID3D12CommandQueue* getCommandQueue() const;
    ID3D12Fence*        getShowFence() const;

private:
    // Helper methods for initialization
    // (createDevice / createCommandQueue moved to D3D12Device class)
    Result createSwapChain(void* windowHandle, uint32_t width, uint32_t height);
    Result createRenderTargetViews();
    Result createCommandAllocators();   // creates both editor and show allocators + lists
    Result createCommandList();
    Result createCopyCommandList();   // Phase C.11: COPY queue cmd list + per-frame allocators
    Result createFence();
    // Returns true only if all three drains (copy, show, editor fences)
    // completed — i.e. the GPU is provably idle. See the definition (#89).
    bool waitForGpu();
    void moveToNextFrame();

    // Helper methods for rendering pipeline
    // Loads a DXC-precompiled shader blob from disk (shaders/<name>.cso next
    // to the executable). See shaders/CMakeLists.txt for the build step.
    Result loadCompiledShader(const std::wstring& csoFilename, ID3DBlob** blob);
    Result createRootSignature();
    Result createPipelineState();
    Result createVertexBuffer();
    Result createConstantBuffer();
    void updateConstantBuffer(const DirectX::XMMATRIX& transform, const DirectX::XMFLOAT4& color, float opacity);

    // Helper methods for textured rendering pipeline
    Result createTexturedRootSignature();
    Result createTexturedPipelineState();
    Result createBlendRootSignature();
    Result createBlendPipelineState();

    // Helper methods for mapping surface rendering pipeline
    Result createMappingSurfaceRootSignature();
    Result createMappingSurfacePipelineState();
    Result createMappingSurfaceVertexBuffer();
    Result createMappingSurfaceConstantBuffer();

    // Helper methods for per-layer effect chain rendering (issue #54).
    // Root signature + VS bytecode are shared across every effect kind; the
    // PS is loaded on first use from EffectKindRegistry::find(kindIdHash).
    Result createEffectRootSignature();
    Result createEffectConstantBufferRing();
    ID3D12PipelineState* getOrBuildEffectPso(std::uint32_t kindIdHash);

    // Helper methods for ImGui
    Result initializeImGui(GLFWwindow* window);
    void shutdownImGui();

    // Device-removed handling: logs the GPU removal reason and latches m_deviceLost.
    // Safe to call from any D3D12 call that returns DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG.
    void handleDeviceLost(HRESULT hr, const char* site);

    // Resolve the device-removed reason after a fence-wait timeout. If the
    // device reports S_OK (the fence timed out before the driver declared a
    // TDR — see the fenceWaitTimeoutMs note), sleep a short grace period and
    // re-query once so a genuine hang is captured with its real reason rather
    // than a misleading 0x0. Returns DXGI_ERROR_DEVICE_HUNG if no device.
    HRESULT removedReasonAfterTimeout();

    // Bounded fence wait for the frame-loop sites (beginShowFrame /
    // moveToNextFrame). Retries a watchdog timeout while the device is
    // healthy (RemovedReason S_OK — CPU starvation, not device loss) and
    // latches device-lost only on a real removal reason, WAIT_FAILED, or
    // retry exhaustion. Returns true when the fence reached `value`; false
    // after handleDeviceLost has run. NOT for the shutdown drains in
    // waitForGpu(), which must stay single-shot bounded (a shutdown path
    // that re-waits on a wedged driver is the #69 teardown hang).
    bool waitFenceOrDeviceLost(ID3D12Fence* fence, uint64_t value,
                               HANDLE event, const char* site);

    // Resolve an abstract TextureRef into a D3D12 SRV descriptor handle.
    // Returns a zero-ptr handle if the reference is invalid or the resource
    // isn't ready; legacy draw methods treat that as a no-op.
    D3D12_GPU_DESCRIPTOR_HANDLE resolveTextureHandle(TextureRef tex) const;

    // Helper methods for screenshot capture
    bool ensureScreenshotStagingBuffer(uint32_t width, uint32_t height);
    bool readbackTextureToPNG(ID3D12Resource* sourceTexture,
                               D3D12_RESOURCE_STATES sourceState,
                               uint32_t width, uint32_t height,
                               const std::string& filepath);
    bool readbackTextureToPixels(ID3D12Resource* sourceTexture,
                                  D3D12_RESOURCE_STATES sourceState,
                                  uint32_t width, uint32_t height,
                                  std::vector<uint8_t>& outPixels);

private:
    static constexpr uint32_t FRAME_COUNT = 2; // Double buffering

    // D3D12 device + command queue — owned by D3D12Device wrapper so the
    // ID3D12Device/ID3D12CommandQueue lifetime is in one place.
    std::unique_ptr<class D3D12Device> m_gpu;

    // Tracy GPU profiling context for the direct command queue.
    // Show-thread only: created after m_gpu is up, destroyed before m_gpu goes down.
    TracyD3D12Ctx m_tracyD3D12Ctx = nullptr;

    // Cached DXGI factory; shared by the main swap chain and per-output
    // swap chains so we don't re-create it every call. Must outlive any
    // swap chain that was created from it.
    ComPtr<IDXGIFactory4> m_dxgiFactory;

    // Swap chain + per-frame render resources. Still on D3D12Renderer
    // because they couple tightly to the back buffer index (which lives
    // here). Extracting these cleanly is Phase B+ work.
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12Resource> m_renderTargets[FRAME_COUNT];

    // Stage 1 A/B-list: editor-side command recording (back-buffer, ImGui).
    ComPtr<ID3D12CommandAllocator>    m_editorAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_editorCmdList;

    // Stage 1 A/B-list: show-side command recording (compose + outputs).
    ComPtr<ID3D12CommandAllocator>    m_showAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_showCmdList;

    // Separate per-role fences. Editor fence gates back-buffer reuse;
    // show fence gates compose-target and output swap-chain resource reuse.
    // Both submit to the same direct command queue (thread-safe for
    // ExecuteCommandLists/Signal per D3D12 spec).
    ComPtr<ID3D12Fence> m_editorFence;
    uint64_t            m_editorFenceValues[FRAME_COUNT];
    // Monotonic editor frame counter. The editor command-allocator ring AND the
    // editor fence ring are keyed on (m_editorFrameCounter % FRAME_COUNT), the
    // SAME key ImGui's backend uses for its per-frame VB/IB ring. Keying on
    // GetCurrentBackBufferIndex() instead (the prior code) desynced from ImGui's
    // ring under no-flip presents and freed a live ImGui buffer. See
    // docs/perf/device-hung-2026-06/diagnosis.md.
    //
    // Slot continuity across the uint64 wrap relies on FRAME_COUNT being a
    // power of two (2^64 is divisible by it, so (counter % FRAME_COUNT) is
    // gap-free as counter rolls UINT64_MAX -> 0). FRAME_COUNT == 2 today; a
    // non-power-of-two FRAME_COUNT would skip slots at the wrap boundary. If
    // FRAME_COUNT ever becomes e.g. 3, key the slot off a separately-wrapped
    // counter (e.g. m_slot = (m_slot + 1) % FRAME_COUNT) instead.
    //
    // NOT the same thing as m_editorFrameTracker (#75), which also counts
    // editor frames: this counter increments AFTER the device-lost early
    // return (it must stay in lockstep with ImGui's ring, which only
    // advances on frames that render), while the tracker increments before
    // it (its begin/end pairing must survive every early return). Merging
    // them breaks one invariant or the other.
    uint64_t m_editorFrameCounter = UINT64_MAX;  // ++ before first use -> 0
    uint32_t m_editorRingSlot = 0;               // m_editorFrameCounter % FRAME_COUNT
    ComPtr<ID3D12Fence> m_showFence;
    uint64_t            m_showFenceValues[FRAME_COUNT];
    void* m_fenceEvent;      // editor thread only (beginEditorFrame / waitForGpu)
    void* m_showFenceEvent;  // show thread only  (beginShowFrame)

    // Phase C.11: async copy queue. Texture uploads (TextureUploader::upload)
    // record into m_copyCommandList and execute on m_gpu->copyQueue() so they
    // overlap with composite/render on the direct queue. The upload fence
    // enforces cross-queue ordering: copy queue Signal(uploadFence, ++value)
    // after Execute, direct queue Wait(uploadFence, value) before its own
    // Execute. Per-frame allocators (FRAME_COUNT) so the previous frame's
    // copy commands aren't reset out from under the GPU.
    // Show-thread copy resources (texture uploads in beginShowFrame/endShowFrame)
    ComPtr<ID3D12CommandAllocator>    m_copyCommandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_copyCommandList;
    // Editor-thread copy resources (mesh uploads in uploadMeshImmediate — editor-only)
    ComPtr<ID3D12CommandAllocator>    m_editorCopyCommandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_editorCopyCommandList;
    ComPtr<ID3D12Fence>               m_uploadFence;
    std::atomic<uint64_t>             m_uploadFenceValue{0};
    bool                              m_uploadsRecordedThisFrame{false};

    // Rendering pipeline objects
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_constantBuffer;
    ComPtr<ID3D12DescriptorHeap> m_cbvHeap;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;
    void* m_constantBufferData;  // DEPRECATED: Mapped pointer (UNUSED - drawColoredQuad uses root constants)

    // ImGui objects
    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;  // Descriptor heap for ImGui fonts/textures

    // Video texture objects (legacy single-texture for backwards compatibility)
    ComPtr<ID3D12Resource> m_videoTexture;        // GPU texture for video frames
    ComPtr<ID3D12Resource> m_videoUploadBuffer;   // Upload heap for texture data
    D3D12_GPU_DESCRIPTOR_HANDLE m_videoTextureGpuHandle{};  // GPU handle for ImGui
    uint32_t m_videoTextureWidth{0};
    uint32_t m_videoTextureHeight{0};
    bool m_videoTextureFirstUpload{true};                   // Track first upload for barrier state
    uint32_t m_srvDescriptorSize{0};

    // Video texture pool owned by TextureUploader (src/render/TextureUploader.hpp).
    // Declared here as std::unique_ptr so we don't have to include the header,
    // keeping the public D3D12Renderer.hpp lean. See destructor for ordering.
    std::unique_ptr<class TextureUploader> m_textureUploader;

    // Phase 5 — non-owning raw pointer to the UploadHeapBufferPool wired in
    // by Renderer::initialize via setUploadHeapPool(). Used by
    // copyUploadBufferToVideoTextureSlot to call recordCopyDone so the
    // buffer's GPU-fence-wait deleter fires correctly. Null until wired;
    // the method degrades to no-op when null (safe because frames are
    // CpuHeap until the pool is available). Renderer owns the pool's
    // lifetime — it constructs it before passing the pointer here and
    // destructs it after D3D12Renderer is shut down.
    class UploadHeapBufferPool* m_uploadHeapPool{nullptr};

    // Deferred video-texture slot frees. Editor thread (on_destroy<VideoTexture>
    // observer, TextSystem) pushes here; show thread drains at beginShowFrame
    // behind the per-slot gate protocol below (#89).
    std::mutex              m_deferredVideoTextureFreesMutex;
    std::vector<uint32_t>   m_deferredVideoTextureFrees;

    // #89: per-video-slot editor read gates. Same hazard class and protocol as
    // ComposeTarget::editorGate (#75): the editor thread grabs slot SRV handles
    // mid-record (getVideoTextureIDForSlot) and submits up to a frame later, so
    // any resource destruction (deferred free, dimension-change recreate) must
    // defer until no recorded-or-in-flight reference remains. Gate writers: the
    // beginShowFrame free drain, the show-side resize branches, and
    // endShowFrame's backstop — show thread only.
    std::array<EditorReadGate, DescriptorHeapLayout::MAX_VIDEO_TEXTURE_SLOTS> m_videoSlotGates;
    // "This closed gate is actively driven this tick" — set by the free drain
    // and the resize branches, consumed+cleared by endShowFrame's backstop.
    // Show-thread only; plain bools are fine.
    std::array<bool, DescriptorHeapLayout::MAX_VIDEO_TEXTURE_SLOTS> m_videoSlotGateDriven{};
    // #89: slot is retiring — a free is pending, refuse NEW references (draws,
    // uploads) so the fence targets captured at retire time stay authoritative.
    // Set/cleared and read on the show thread only (the editor never uploads
    // to a slot after scheduling its free: the owning component is gone).
    std::array<bool, DescriptorHeapLayout::MAX_VIDEO_TEXTURE_SLOTS> m_videoSlotRetiring{};
    // Show-thread staging for deferred frees. The free path is deliberately
    // NON-blocking (a waitForGpu here hitches the projector output at section
    // breaks / clip deletes — the hottest teardown moments): at retire time we
    // enqueue fence signals covering everything submitted that could still
    // reference the slot, and free once GetCompletedValue passes them.
    struct PendingSlotFree {
        uint32_t slot;
        uint64_t copyTarget{0};        // m_retireCopyFence value signaled at retire
        uint64_t directTarget{0};      // m_retireDirectFence value signaled at mayMutate
        bool     directCaptured{false};
    };
    std::vector<PendingSlotFree> m_pendingSlotFreesShow;
    // #89 retire witness fences. The per-frame fences can't serve as completion
    // witnesses: endShowFrame's per-ring-slot values are DUPLICATED across
    // consecutive in-flight frames (1,1,2,2,...), so "completed >= last
    // signaled" can be satisfied by an older frame's identical signal while the
    // newest still executes. These fences are signaled ONLY by the show thread's
    // free drain with strictly monotonic private counters — one Signal enqueued
    // on a queue completes only after all work enqueued on it beforehand.
    ComPtr<ID3D12Fence> m_retireDirectFence;   // direct queue: show + editor lists
    ComPtr<ID3D12Fence> m_retireCopyFence;     // copy queue: texture uploads
    uint64_t m_retireDirectValue{0};           // show-thread only
    uint64_t m_retireCopyValue{0};             // show-thread only
    // Slots freed by this tick's drain; endShowFrame's backstop guard clears
    // their retiring flags AFTER the frame's recording is done, so a stale
    // RenderFrame built before the drain can't upload into a slot that was
    // freed and re-allocated within the same tick. Show-thread only.
    std::vector<uint32_t> m_slotsFreedThisTick;
    // Last values signaled on the per-frame fences, used by waitForGpu to floor
    // its drain values above every ALREADY-ENQUEUED signal (GetCompletedValue+1
    // alone can collide with an in-flight signal's value, making the drain wait
    // fire before later-enqueued lists finish). Atomics: each is written by its
    // owning thread (show: endShowFrame / editor: moveToNextFrame) and read by
    // waitForGpu from either thread.
    std::atomic<uint64_t> m_lastSignaledShowFence{0};
    std::atomic<uint64_t> m_lastSignaledEditorFence{0};
    // Serializes (increment m_uploadFenceValue, Signal m_uploadFence) pairs.
    // Without it the editor's immediate-upload path and the show thread's
    // endShowFrame can enqueue their Signals in the OPPOSITE order of their
    // values; ID3D12Fence SETS the value (no max), so the completed value
    // regresses and a direct-queue Wait on the higher value stalls the show
    // queue until the next upload happens to re-signal past it.
    std::mutex m_uploadFenceSignalMutex;

    // Mesh vertex/index buffer pool (Phase 4+). Wired up in Phase 7 lifecycle.
    std::unique_ptr<class MeshUploader> m_meshUploader;
    // Slot for the shared default 16:9 quad used by flat (mesh-less) screens.
    // Uploaded once in ensureStageTarget; never freed (MeshUploader::shutdown clears it).
    uint32_t m_defaultScreenMeshSlot{UINT32_MAX};

    // Textured rendering pipeline (for drawTexturedQuad)
    ComPtr<ID3D12RootSignature> m_texturedRootSignature;
    ComPtr<ID3D12PipelineState> m_texturedPipelineState;         // Normal blend
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateAdd;      // Additive blend
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateMultiply; // Multiply blend
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateScreen;   // Screen blend

    // Shader-based blend pipeline (Overlay / SoftLight / HardLight / ColorDodge /
    // ColorBurn / Darken / Lighten / Difference / Exclusion). PS samples both
    // fg (t0) and a snapshot of the current compose target (t1), computes the
    // blend in HLSL, writes final color (BlendEnable=FALSE on the PSO).
    ComPtr<ID3D12RootSignature> m_blendRootSignature;
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateBlend;

    // ---------------------------------------------------------------------
    // Phase C.12 #5 — OCIO-spliced PSO variants.
    //
    // OcioManager (held as a non-owning pointer; owned by Engine) is the
    // source of truth for what input transform applies to a given clip and
    // what display+view applies to a given output. When set, draw paths
    // look up an OCIO processor + use the runtime-compiled PSO that has
    // the OCIO HLSL function spliced in. When null, the offline-compiled
    // PSOs (m_texturedPipelineState etc.) are used unchanged.
    //
    // Caches are keyed on the OCIO function name (already canonicalized by
    // OcioManager::bakeProcessor — see OCIO underscore-collapsing note in
    // C.12 #4). Lazy-built on first use; persist for the lifetime of the
    // active OCIO config; cleared when the config reloads.
    // ---------------------------------------------------------------------
    OcioManager* m_ocioManager{nullptr};
    RuntimeShaderCompiler m_runtimeCompiler;

    // 5 PSOs per input transform: Normal / Add / Multiply / Screen + Blend.
    struct OcioCompositePsoSet {
        ComPtr<ID3D12PipelineState> psoNormal;
        ComPtr<ID3D12PipelineState> psoAdd;
        ComPtr<ID3D12PipelineState> psoMultiply;
        ComPtr<ID3D12PipelineState> psoScreen;
        ComPtr<ID3D12PipelineState> psoBlend;          // shader-blend (BlendEnable=FALSE)
    };
    std::unordered_map<std::string, OcioCompositePsoSet> m_ocioCompositePsoCache;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> m_ocioMappingSurfacePsoCache;
    ComPtr<ID3D12PipelineState>          m_ocioCapturePipelineState;  // overrides m_capturePipelineState when present
    std::shared_ptr<const OcioGpuProcessor> m_activeInputProcessor;   // currently bound input transform (subtask 6 makes per-clip)
    std::shared_ptr<const OcioGpuProcessor> m_activeDisplayProcessor; // currently bound display transform (subtask 8 makes per-output)
    std::shared_ptr<const OcioGpuProcessor> m_activeCaptureProcessor; // pinned to the canonical sRGB display+view

    // Cached HLSL source files loaded from <exe_dir>/shaders/. Re-read on
    // OCIO config reload so the same file edited on disk gets picked up
    // (developer convenience).
    std::string m_compositePsSource;
    std::string m_compositeBlendPsSource;
    std::string m_mappingSurfacePsSource;
    std::string m_acesCapturePsSource;

    // Mapping surface rendering pipeline (for projection mapping)
    ComPtr<ID3D12RootSignature> m_mappingSurfaceRootSignature;
    ComPtr<ID3D12PipelineState> m_mappingSurfacePipelineState;
    ComPtr<ID3D12Resource> m_mappingSurfaceVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_mappingSurfaceVertexBufferView;

    // Per-frame ring buffer of constant buffer slots (CRIT-04 fix).
    // One slot per drawOutputSurface call. Reset each frame at beginShowFrame().
    // Fence sync (moveToNextFrame) ensures the GPU has finished reading the
    // previous use of the current frame's region before we overwrite.
    static constexpr uint32_t MAX_MAPPING_SURFACES_PER_FRAME = 8192;
    ComPtr<ID3D12Resource> m_mappingSurfaceConstantRing;
    uint8_t* m_mappingSurfaceConstantRingMapped{nullptr};
    uint32_t m_mappingSurfaceSlotSize{0};          // 256-aligned sizeof(MappingSurfaceConstants)
    uint32_t m_mappingSurfaceDrawIndex{0};         // Resets to 0 each frame
    bool m_mappingSurfaceOverflowed{false};        // Latched until next frame — prevents log spam

    // --------------------------------------------------------------------
    // Per-layer effect chain (issue #54 — PASS 1.5 of CompositorSystem).
    //
    // One shared root signature (SRV t0 + CBV b0 + static sampler s0). One
    // shared VS .cso (effect_vs.cso, fullscreen triangle). One PSO per
    // effect kind, loaded lazily on first drawEffectPass for that kind
    // from EffectKindRegistry::find(kindIdHash)->shaderPath. CBuffer ring
    // mirrors the mapping-surface CRIT-04 fix: 256-byte slots × per-frame
    // budget × FRAME_COUNT, persistently mapped upload heap. --------------------------------------------------------------------
    const effects::EffectKindRegistry* m_effectKindRegistry{nullptr};

    ComPtr<ID3D12RootSignature> m_effectRootSignature;
    ComPtr<ID3DBlob>            m_effectVsBlob;
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> m_effectPsoCache;

    static constexpr uint32_t MAX_EFFECT_DRAWS_PER_FRAME = 256;
    static constexpr uint32_t EFFECT_CBUFFER_SLOT_SIZE   = 256;
    ComPtr<ID3D12Resource> m_effectCbufferRing;
    uint8_t*               m_effectCbufferRingMapped{nullptr};
    uint32_t               m_effectDrawIndex{0};      // resets each frame
    bool                   m_effectOverflowed{false}; // one-shot log

    // Mapping surface constant buffer structure (must match HLSL)
    struct MappingSurfaceConstants {
        DirectX::XMFLOAT4 corners[4];      // Corner positions (xy used, zw padding)
        DirectX::XMFLOAT4 sourceUVs[4];    // Source UV coordinates (xy used, zw padding)
        float softEdgeLeft;
        float softEdgeRight;
        float softEdgeTop;
        float softEdgeBottom;
        float brightness;
        float gamma;
        float opacity;
        float padding1;
    };

    // Constant buffer structure (must match HLSL common.hlsli LayerConstants).
    // 28 dwords (112 bytes) after ADR-0021 M3 added uvRect for Tiled source
    // cropping. Three root signatures (1269 / 1990 / 2209) bumped 24 → 28 to
    // match; three send sites (1515 / 2449 / 2488) match.
    struct LayerConstants {
        DirectX::XMFLOAT4X4 transform;
        DirectX::XMFLOAT4 color;
        float opacity;
        uint32_t blendMode;    // 0=Normal, 1=Add, 2=Multiply, 3=Screen, ...
        uint32_t colorSpace;   // 0=Linear, 1=YCoCg_scaled (matches TextureColorSpace + COLOR_SPACE_* in common.hlsli)
        float padding3;
        // Source-UV crop for content-routing Tiled mode (ADR-0021 M3). Identity
        // (0,0,1,1) = full source; non-identity sub-region carves a feed-map
        // slice. Applied in composite_vs.hlsl as
        // `texCoord = texCoord * uvRect.zw + uvRect.xy`.
        DirectX::XMFLOAT4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
    };

    // Offscreen compose targets (for multi-clip compositing, one per screen).
    // Phase C.12 #3 changed the format from R8G8B8A8_UNORM to
    // R16G16B16A16_FLOAT — the linear ACEScg working space the OCIO display
    // transform in mapping_surface_ps consumes when it writes to the
    // (still-UNORM8) swap chain.
    struct ComposeTarget {
        // Stage 3d: triple-buffered backing resources. Show thread writes to
        // resources[writeIndex % TRIPLE] each tick, then stores writeIndex into
        // lastStableIndex (release). Editor reads at lastStableIndex.load(acquire).
        // Sub-index 0 reuses the existing composeTargetSlot heap entry for
        // backward compatibility; sub-indices 1+2 use composeTargetTripleSlot.
        static constexpr uint32_t TRIPLE = 3;
        ComPtr<ID3D12Resource>       resources[TRIPLE];
        ComPtr<ID3D12DescriptorHeap> rtvHeaps[TRIPLE];
        D3D12_GPU_DESCRIPTOR_HANDLE  srvHandles[TRIPLE]{};
        std::atomic<uint32_t>        lastStableIndex{UINT32_MAX}; // UINT32_MAX = no stable frame yet
        uint32_t                     writeIndex{0};               // show thread only; next sub to write

        // #75: gates editor readers across resizeComposeTarget's resource
        // swap + descriptor rewrite. Triple-buffering (above) only guards RT
        // *contents*; this guards *structure*. Editor-facing accessors check
        // it before touching handles/dims; the show thread closes it, waits
        // for in-flight editor frames, mutates, reopens. Show-side render
        // paths never check it — the swap is synchronous on their thread.
        EditorReadGate editorGate;

        // #75 liveness backstop bookkeeping (show thread only). Set by
        // resizeComposeTarget whenever it reaches its gate logic; cleared by
        // endShowFrame each tick. A gate left closed with no resize call
        // re-asserting it that tick was abandoned (dims reverted mid-deferral,
        // screen deleted, invalid snapshot dims) — endShowFrame reopens it so
        // the editor preview can never be stranded blank.
        bool resizeAttemptedThisTick{false};

        // std::atomic is not moveable, so provide an explicit move
        // constructor — std::vector requires MoveInsertable even though
        // m_composeTargets is reserve()d to MAX_COMPOSE_TARGETS at
        // initialize() and never reallocates. Targets are created and
        // resized at runtime on the show thread while the editor reads
        // (#75); the reserve + m_composeTargetCount publish + editorGate
        // carry that safety, not this move constructor.
        ComposeTarget() = default;
        ComposeTarget(ComposeTarget&& o) noexcept
            : srvHandles{o.srvHandles[0], o.srvHandles[1], o.srvHandles[2]}
            , lastStableIndex(o.lastStableIndex.load(std::memory_order_relaxed))
            , writeIndex(o.writeIndex)
            , editorGate(std::move(o.editorGate))
            , resizeAttemptedThisTick(o.resizeAttemptedThisTick)
            , snapshotResource(std::move(o.snapshotResource))
            , snapshotSrvHandle(o.snapshotSrvHandle)
            , snapshotSrvSlot(o.snapshotSrvSlot)
            , width(o.width)
            , height(o.height)
            , ready(o.ready)
        {
            for (int i = 0; i < TRIPLE; ++i) {
                resources[i] = std::move(o.resources[i]);
                rtvHeaps[i]  = std::move(o.rtvHeaps[i]);
            }
        }
        ComposeTarget& operator=(ComposeTarget&&) = delete;
        ComposeTarget(const ComposeTarget&) = delete;
        ComposeTarget& operator=(const ComposeTarget&) = delete;

        // Helpers for callers that work with the current write sub-resource.
        ComPtr<ID3D12Resource>&       writeResource() { return resources[writeIndex % TRIPLE]; }
        ComPtr<ID3D12DescriptorHeap>& writeRtvHeap () { return rtvHeaps[writeIndex % TRIPLE]; }

        // SRV handle for the last stably-rendered sub-resource (editor reads this).
        D3D12_GPU_DESCRIPTOR_HANDLE stableSrvHandle() const {
            const uint32_t idx = lastStableIndex.load(std::memory_order_acquire);
            return (idx != UINT32_MAX) ? srvHandles[idx % TRIPLE] : D3D12_GPU_DESCRIPTOR_HANDLE{};
        }

        // Read-back copy for shader-blend modes (Overlay, Difference, etc.).
        // Snapshotted from writeResource() just before each blend draw.
        // Lazily allocated; see ensureSnapshotResource().
        ComPtr<ID3D12Resource>      snapshotResource;
        D3D12_GPU_DESCRIPTOR_HANDLE snapshotSrvHandle{};
        uint32_t snapshotSrvSlot{0}; // reserved heap slot (populated lazily)
        uint32_t width{0};
        uint32_t height{0};
        bool ready{false};
    };
    std::vector<ComposeTarget> m_composeTargets;
    uint32_t m_currentComposeTargetSlot{0};  // Currently active slot during rendering

    // #75: guard for editor-thread compose-target accessors: published-count
    // bound check + per-target editorGate. Defined in D3D12Renderer.cpp.
    bool composeTargetEditorReadable(uint32_t slot) const;

    // #75: count of fully-initialized compose targets, published with release
    // semantics at the end of createComposeTarget. Editor-facing accessors
    // bound-check against THIS, never m_composeTargets.size() — size() grows
    // at emplace_back, before the element is initialized, and reading it
    // cross-thread races the show thread's push. The vector is reserve()d to
    // MAX_COMPOSE_TARGETS in initialize() so element addresses are stable.
    std::atomic<uint32_t> m_composeTargetCount{0};

    // #75: editor-frame record/submit progress, consumed by the per-target
    // editorGate (see EditorReadGate.hpp for the protocol).
    EditorFrameTracker m_editorFrameTracker;

    // Double-buffered offscreen render target for the editor 3D stage view.
    // Color: RGBA16F + RTV + SRV in main heap. Depth: D32_FLOAT + DSV (own heap).
    // frameCB: persistently-mapped UPLOAD-heap buffer for FrameCB (viewProj).
    // Editor thread only — never touched by the show thread.
    struct StageRenderTarget {
        ComPtr<ID3D12Resource>       color[FRAME_COUNT];
        ComPtr<ID3D12Resource>       depth[FRAME_COUNT];
        ComPtr<ID3D12DescriptorHeap> rtvHeap;   // FRAME_COUNT RTVs
        ComPtr<ID3D12DescriptorHeap> dsvHeap;   // FRAME_COUNT DSVs (never shader-visible)
        uint32_t srvSlots[FRAME_COUNT]{UINT32_MAX, UINT32_MAX}; // SRV slots in m_imguiSrvHeap
        // Per-frame FrameCB buffers (one per FRAME_COUNT slot, 256-byte aligned).
        // Frame N+2's CPU recording must not overwrite frame N+1's buffer while
        // the GPU is still reading it; indexing by m_currentBackBufferIndex gives
        // each in-flight frame its own copy, matching moveToNextFrame's fence logic.
        ComPtr<ID3D12Resource> frameCB[FRAME_COUNT];
        void*                  frameCBMapped[FRAME_COUNT]{nullptr, nullptr};
        uint32_t width{0};
        uint32_t height{0};
        bool ready{false};
    };
    StageRenderTarget m_stageTarget;
    bool createStageRenderTarget(uint32_t width, uint32_t height);

    // Straight-alpha sibling of m_stageTarget. The 3D mesh pass writes
    // premultiplied RGBA into m_stageTarget; applyStageDePremul() then
    // runs a fullscreen pass that samples the premul RT (via t0) and
    // writes straight-alpha RGBA into this sibling. ImGui samples THIS
    // RT, so its straight-alpha composite is mathematically correct.
    // Color only — no depth, no per-frame CB. RTV in own heap; SRVs in
    // m_imguiSrvHeap at DescriptorHeapLayout::stageTargetStraightSlot().
    struct StageRenderTargetStraight {
        ComPtr<ID3D12Resource>       color[FRAME_COUNT];
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        uint32_t srvSlots[FRAME_COUNT]{UINT32_MAX, UINT32_MAX};
        uint32_t width{0};
        uint32_t height{0};
        bool ready{false};
    };
    StageRenderTargetStraight m_stageTargetStraight;
    bool createStageRenderTargetStraight(uint32_t width, uint32_t height);

    // Stage mesh PSO + root signature (depth-enabled, cull-none). Two
    // PSOs share the root signature: the opaque one writes depth and
    // does no blending; the transparent one does premul-over blending
    // and disables depth-write for back-to-front sorting.
    ComPtr<ID3D12RootSignature> m_stageMeshRootSignature;
    ComPtr<ID3D12PipelineState> m_stageMeshPipelineState;
    ComPtr<ID3D12PipelineState> m_stageMeshPipelineStateTransparent;
    bool createStageMeshPSO();

    // De-premultiply fullscreen pass — its own root signature (single
    // SRV table + static sampler, no CBV) so it stays independent of
    // the effect-chain root sig.
    ComPtr<ID3D12RootSignature> m_stageDePremulRootSignature;
    ComPtr<ID3D12PipelineState> m_stageDePremulPipelineState;
    bool createStageDePremulPSO();

    /**
     * Lazily allocate the FP16 snapshot resource for a compose target and
     * register its SRV in the descriptor heap at the slot reserved by
     * createComposeTarget. Returns true if the snapshot is valid afterwards
     * (already-allocated or freshly created); false if the GPU resource
     * couldn't be created (out-of-VRAM, device removed, etc.).
     */
    bool ensureSnapshotResource(ComposeTarget& target);

    // ---------------------------------------------------------------------
    // Phase C.12 #3 — capture buffer + tone-mapping pass.
    //
    // The compose target is FP16 linear; integration-test goldens hash a
    // bytewise UNORM8 snapshot of "what an sRGB monitor would show". To
    // keep that hash portable across machines (vs hashing raw FP16 bits
    // which are hardware-quantization-sensitive), we render the compose
    // SRV through aces_capture_ps.hlsl into a UNORM8 capture texture and
    // read THAT back. Subtask 3 ships an sRGB-only stub (linearToSrgb);
    // subtask 5 swaps it for an OCIO-emitted display transform.
    // ---------------------------------------------------------------------
    ComPtr<ID3D12Resource>       m_captureResource;
    ComPtr<ID3D12DescriptorHeap> m_captureRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_captureRtv{};
    // Capture-only allocator + list. The capture path runs on the show thread
    // (CaptureBroker drain, pre-beginShowFrame) and used to Reset/record the
    // EDITOR allocator + cmdlist, racing the editor thread's ImGui recording —
    // allocator Reset fails mid-recording ("[Capture] command allocator Reset
    // failed", flaky golden-hash tests under CPU load), and a successful Reset
    // there would be UB. Same isolation pattern as m_editorCopyCommand* vs the
    // show copy list. m_captureMutex serializes the two capture entry points
    // (tonemapAndReadbackComposeTarget, readbackTextureToPixels) against each
    // other — all current callers funnel through the show thread's
    // CaptureBroker drain, but the IRenderer capture API is public and
    // nothing stops a future editor-thread caller. One allocator (no ring)
    // suffices because each capture fully drains the GPU before returning.
    ComPtr<ID3D12CommandAllocator>    m_captureAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_captureCmdList;
    std::mutex                        m_captureMutex;
    ComPtr<ID3D12RootSignature>  m_captureRootSignature;
    ComPtr<ID3D12PipelineState>  m_capturePipelineState;
    uint32_t                     m_captureWidth{0};
    uint32_t                     m_captureHeight{0};

    bool createCaptureRootSignatureAndPSO();
    bool ensureCaptureResource(uint32_t width, uint32_t height);

    // Calibration overlay PSO (calibration_overlay_vs/ps.hlsl)
    struct alignas(16) CalibrationConstants {
        std::array<float, 32> pts; // packed float4[8]: pts[2i], pts[2i+1] → float4[i]
        int   numPts{0};
        int   activeIdx{-1};
        // 1 = full-frame quadrant checkerboard centered on the active crosshair
        // (precision cursor). 0 = standard small crosshair markers.
        int   checkerboard{0};
        float pad{};
    };
    ComPtr<ID3D12RootSignature>  m_calibRootSignature;
    ComPtr<ID3D12PipelineState>  m_calibPipelineState;

    // Mesh-triangle PSO (proper barycentric UV for projector output mesh rendering).
    ComPtr<ID3D12RootSignature>  m_meshTriRootSignature;
    ComPtr<ID3D12PipelineState>  m_meshTriPipelineState;
    bool createMeshTrianglePSO();
    bool createCalibrationOverlayPSO();
    bool tonemapAndReadbackComposeTarget(uint32_t slot,
                                          uint32_t& outWidth,
                                          uint32_t& outHeight,
                                          std::vector<uint8_t>& outPixels);

    // -------------------------------------------------------------------
    // Phase C.12 #5 — runtime-compiled OCIO PSO helpers.
    // -------------------------------------------------------------------

    /** Read shaders/<filename> from <exe_dir>/shaders/. Returns empty on miss. */
    std::string loadShaderSourceFromExeDir(const std::string& filename);
    /** Read all .hlsl + .hlsli sources we need into m_*Source members. */
    bool loadOcioShaderSources();

    /** Build the 5-PSO OcioCompositePsoSet for an input transform.
     *  Splices the input-fn HLSL onto composite_ps.hlsl + composite_blend_ps.hlsl,
     *  compiles each, then constructs PSOs for the 4 fixed-function blend modes
     *  + the 1 shader-blend mode. Returns nullptr on failure (logs).
     */
    const OcioCompositePsoSet* getOrBuildOcioCompositePsoSet(const OcioGpuProcessor& input);

    /** Build the mapping-surface PSO for a display+view processor. */
    ComPtr<ID3D12PipelineState> getOrBuildOcioMappingSurfacePso(const OcioGpuProcessor& display);

    /** Compile + return the capture PSO for a fixed sRGB display+view. */
    ComPtr<ID3D12PipelineState> buildOcioCapturePso(const OcioGpuProcessor& display);

    // Active command list — thread_local, lives in D3D12Renderer.cpp.
    // Set by begin{Show,Editor}Frame; read by all draw methods.
    // No member needed here; the thread_local is accessed via activeCmdList()
    // (a file-static helper in the .cpp). Not a struct member because
    // thread_local members are not allowed in C++.

    // Per-output swap chains for physical displays (Phase C #1).
    // Each OutputWindow owns its own borderless GLFW window, DXGI swap chain,
    // and set of back-buffer RTVs. Draws record into the thread-local active list.
    struct OutputWindow {
        GLFWwindow* window{nullptr};       // Owned GLFW window (nullptr when freed)
        void* hwnd{nullptr};               // HWND kept as void* to avoid leaking windows.h
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12Resource> renderTargets[FRAME_COUNT];
        uint32_t width{0};
        uint32_t height{0};
        uint32_t currentBackBufferIndex{0};
        bool active{false};                // false = freed slot, reusable
    };
    std::vector<OutputWindow> m_outputWindows;
    uint32_t m_currentOutputSlot{UINT32_MAX};  // Set during begin/endOutputFrame

    // State
    // m_currentBackBufferIndex: editor thread only (beginEditorFrame / endEditorFrame).
    // m_showBackBufferIndex:    show thread only  (beginShowFrame / endShowFrame).
    // Both are set from GetCurrentBackBufferIndex() at the start of their respective
    // begin calls. Never cross-access: that would be a data race.
    uint32_t m_currentBackBufferIndex;
    uint32_t m_showBackBufferIndex{0};
    uint32_t m_rtvDescriptorSize;
    uint32_t m_width;
    uint32_t m_height;
    bool m_initialized;
    std::atomic<bool> m_deviceLost{false}; // Latched on first device-removed detection. Written from show or editor thread; read from both.
    // Set once during Engine::initialize (headless only), read from whichever
    // thread wins the handleDeviceLost CAS. See setExitProcessOnDeviceLost().
    std::atomic<bool> m_exitProcessOnDeviceLost{false};
    // GetDeviceRemovedReason() result; available via getDeviceLostReason() and
    // read (acquire) by the shutdown drain gate. Atomic because multiple
    // threads can hit device-lost concurrently and a third thread reads it.
    std::atomic<HRESULT> m_deviceLostReason{S_OK};

    // Descriptor heap caching — thread_local, lives in D3D12Renderer.cpp.
    // Each thread tracks its own last-bound heap to avoid redundant calls.

    // Screenshot capture staging buffer
    ComPtr<ID3D12Resource> m_screenshotStagingBuffer;
    uint32_t m_screenshotStagingWidth{0};
    uint32_t m_screenshotStagingHeight{0};
    uint64_t m_screenshotStagingRowPitch{0};
};

} // namespace entity
