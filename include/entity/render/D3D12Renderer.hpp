#pragma once

/**
 * D3D12Renderer - Direct3D 12 rendering backend
 *
 * Handles D3D12 device initialization, swap chain management, and rendering.
 * This is a minimal implementation to get something on screen.
 */

#include "entity/core/Types.hpp"
#include "entity/render/IRenderer.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct GLFWwindow;

namespace entity {

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
    bool   isInitialized() const override { return m_initialized; }
    bool   isDeviceLost() const override  { return m_deviceLost; }

    void     beginFrame() override;
    void     endFrame() override;
    void     clear(float r, float g, float b, float a) override;
    uint32_t getCurrentBackBufferIndex() const override { return m_currentBackBufferIndex; }
    void     beginImGuiFrame() override;
    void     endImGuiFrame() override;

    uint32_t   allocateVideoTextureSlot() override;
    void       freeVideoTextureSlot(uint32_t slot) override;
    bool       uploadVideoFrameToSlot(uint32_t slot,
                                      const uint8_t* rgba,
                                      uint32_t width,
                                      uint32_t height) override;
    bool       uploadVideoFrameToSlot(uint32_t slot,
                                      const uint8_t* data,
                                      uint32_t width,
                                      uint32_t height,
                                      TextureFormat format) override;
    TextureRef getVideoTexture(uint32_t slot) const override;

    uint32_t   createComposeTarget(uint32_t width, uint32_t height) override;
    void       beginComposeTarget(uint32_t slot = 0) override;
    void       endComposeTarget() override;
    TextureRef getComposeTargetTexture(uint32_t slot = 0) const override;
    void*      getComposeTargetTextureID(uint32_t slot = 0) const override;
    uint32_t   getComposeTargetWidth(uint32_t slot = 0) const override;
    uint32_t   getComposeTargetHeight(uint32_t slot = 0) const override;
    bool       isComposeTargetReady(uint32_t slot = 0) const override;

    void drawColoredQuad(const glm::mat4& transform,
                          const glm::vec4& color,
                          float opacity) override;

    void drawTexturedQuad(TextureRef texture,
                           const glm::mat4& transform,
                           float opacity,
                           BlendMode blendMode = BlendMode::Normal) override;

    void drawMappingSurface(TextureRef texture,
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
                                TextureFormat format = TextureFormat::RGBA8_UNORM);

    /** Draw a textured quad with D3D12 SRV + XMMATRIX. Legacy. */
    void drawTexturedQuad(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                          const DirectX::XMMATRIX& transform,
                          float opacity,
                          BlendMode blendMode = BlendMode::Normal);

    /** Draw a mapping surface with D3D12 SRV + XMFLOAT args. Legacy. */
    void drawMappingSurface(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
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

private:
    // Helper methods for initialization
    // (createDevice / createCommandQueue moved to D3D12Device class)
    Result createSwapChain(void* windowHandle, uint32_t width, uint32_t height);
    Result createRenderTargetViews();
    Result createCommandAllocators();
    Result createCommandList();
    Result createFence();
    void waitForGpu();
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

    // Helper methods for ImGui
    Result initializeImGui(GLFWwindow* window);
    void shutdownImGui();

    // Device-removed handling: logs the GPU removal reason and latches m_deviceLost.
    // Safe to call from any D3D12 call that returns DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG.
    void handleDeviceLost(HRESULT hr, const char* site);

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
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // Synchronization objects
    ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValues[FRAME_COUNT];
    void* m_fenceEvent;

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

    // Mapping surface rendering pipeline (for projection mapping)
    ComPtr<ID3D12RootSignature> m_mappingSurfaceRootSignature;
    ComPtr<ID3D12PipelineState> m_mappingSurfacePipelineState;
    ComPtr<ID3D12Resource> m_mappingSurfaceVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_mappingSurfaceVertexBufferView;

    // Per-frame ring buffer of constant buffer slots (CRIT-04 fix).
    // One slot per drawMappingSurface call. Reset each frame at beginFrame().
    // Fence sync (moveToNextFrame) ensures the GPU has finished reading the
    // previous use of the current frame's region before we overwrite.
    static constexpr uint32_t MAX_MAPPING_SURFACES_PER_FRAME = 64;
    ComPtr<ID3D12Resource> m_mappingSurfaceConstantRing;
    uint8_t* m_mappingSurfaceConstantRingMapped{nullptr};
    uint32_t m_mappingSurfaceSlotSize{0};          // 256-aligned sizeof(MappingSurfaceConstants)
    uint32_t m_mappingSurfaceDrawIndex{0};         // Resets to 0 each frame
    bool m_mappingSurfaceOverflowed{false};        // Latched until next frame — prevents log spam

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

    // Constant buffer structure (must match HLSL)
    struct LayerConstants {
        DirectX::XMFLOAT4X4 transform;
        DirectX::XMFLOAT4 color;
        float opacity;
        uint32_t blendMode;    // 0=Normal, 1=Add, 2=Multiply, 3=Screen
        float padding2;
        float padding3;
    };

    // Offscreen compose targets (for multi-clip compositing, one per screen)
    struct ComposeTarget {
        ComPtr<ID3D12Resource> resource;               // Render target texture
        ComPtr<ID3D12DescriptorHeap> rtvHeap;         // RTV for rendering to it
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{};      // SRV for sampling
        // Read-back copy of `resource` taken just before each shader-blend
        // draw, sampled as t1 in composite_blend_ps. Resting state is
        // PIXEL_SHADER_RESOURCE; transitions to COPY_DEST and back during the
        // snapshot copy in drawTexturedQuad.
        ComPtr<ID3D12Resource> snapshotResource;
        D3D12_GPU_DESCRIPTOR_HANDLE snapshotSrvHandle{};
        uint32_t width{0};
        uint32_t height{0};
        bool ready{false};
    };
    std::vector<ComposeTarget> m_composeTargets;
    uint32_t m_currentComposeTargetSlot{0};  // Currently active slot during rendering

    // Per-output swap chains for physical displays (Phase C #1).
    // Each OutputWindow owns its own borderless GLFW window, DXGI swap chain,
    // and set of back-buffer RTVs. Draws go into the shared main command list;
    // Present happens inside endFrame() alongside the main swap chain.
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
    uint32_t m_currentBackBufferIndex;
    uint32_t m_rtvDescriptorSize;
    uint32_t m_width;
    uint32_t m_height;
    bool m_initialized;
    bool m_deviceLost{false};  // Set when GPU device-removed is detected; Engine shuts down cleanly.

    // Descriptor heap caching (to avoid redundant SetDescriptorHeaps calls)
    ID3D12DescriptorHeap* m_currentDescriptorHeap{nullptr};

    // Screenshot capture staging buffer
    ComPtr<ID3D12Resource> m_screenshotStagingBuffer;
    uint32_t m_screenshotStagingWidth{0};
    uint32_t m_screenshotStagingHeight{0};
    uint64_t m_screenshotStagingRowPitch{0};
};

} // namespace entity
