#pragma once

/**
 * D3D12Renderer - Direct3D 12 rendering backend
 *
 * Handles D3D12 device initialization, swap chain management, and rendering.
 * This is a minimal implementation to get something on screen.
 */

#include "entity/core/Types.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>

// Forward declarations
struct GLFWwindow;

namespace entity {

using Microsoft::WRL::ComPtr;

class D3D12Renderer {
public:
    // Maximum number of video texture slots (layers)
    static constexpr uint32_t MAX_VIDEO_TEXTURE_SLOTS = 16;

    D3D12Renderer();
    ~D3D12Renderer();

    /**
     * Initialize the D3D12 device, command queue, and swap chain.
     */
    Result initialize(GLFWwindow* window, uint32_t width, uint32_t height);

    /**
     * Shutdown and release all D3D12 resources.
     */
    void shutdown();

    /**
     * Resize swap chain buffers.
     */
    Result resize(uint32_t width, uint32_t height);

    /**
     * Begin a new frame.
     */
    void beginFrame();

    /**
     * End frame and present to screen.
     */
    void endFrame();

    /**
     * Clear the current render target to a color.
     */
    void clear(float r, float g, float b, float a);

    /**
     * Get the current back buffer index.
     */
    uint32_t getCurrentBackBufferIndex() const { return m_currentBackBufferIndex; }

    /**
     * Check if renderer is initialized.
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * Draw a colored quad with transform and opacity.
     */
    void drawColoredQuad(const DirectX::XMMATRIX& transform,
                         const DirectX::XMFLOAT4& color,
                         float opacity);

    /**
     * Begin ImGui frame for UI rendering.
     */
    void beginImGuiFrame();

    /**
     * End ImGui frame and render UI.
     */
    void endImGuiFrame();

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

    // ========================================================================
    // Multi-Texture Support (for multi-layer compositing)
    // ========================================================================

    /**
     * Allocate a video texture slot.
     * @return Slot index (0 to MAX_VIDEO_TEXTURE_SLOTS-1), or UINT32_MAX if none available
     */
    uint32_t allocateVideoTextureSlot();

    /**
     * Free a previously allocated video texture slot.
     * @param slot The slot index to free
     */
    void freeVideoTextureSlot(uint32_t slot);

    /**
     * Upload RGBA video frame data to a specific texture slot.
     * @param slot The slot index (from allocateVideoTextureSlot)
     * @param rgbaData Pointer to RGBA pixel data
     * @param width Width in pixels
     * @param height Height in pixels
     * @param outSrvHandle Output: GPU descriptor handle for the texture
     * @return true on success, false on failure
     */
    bool uploadVideoFrameToSlot(uint32_t slot,
                                const uint8_t* rgbaData,
                                uint32_t width, uint32_t height,
                                D3D12_GPU_DESCRIPTOR_HANDLE* outSrvHandle);

    /**
     * Draw a textured quad with transform and opacity.
     * Uses the composite pixel shader for texture sampling.
     * @param textureSrv GPU descriptor handle for the texture
     * @param transform Transformation matrix
     * @param opacity Layer opacity (0.0 - 1.0)
     * @param blendMode Blend mode (default: Normal)
     */
    void drawTexturedQuad(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                          const DirectX::XMMATRIX& transform,
                          float opacity,
                          BlendMode blendMode = BlendMode::Normal);

    /**
     * Get the D3D12 device (for external texture creation if needed).
     */
    ID3D12Device* getDevice() const { return m_device.Get(); }

    // ========================================================================
    // Mapping Surface Rendering (for projection mapping)
    // ========================================================================

    /**
     * Render a video texture through a mapping surface with perspective warping.
     * @param textureSrv GPU descriptor handle for the video texture
     * @param corners Array of 4 corner positions in clip space (-1 to 1), order: TL, TR, BR, BL
     * @param sourceUVs Array of 4 source UV coordinates, order: TL, TR, BR, BL
     * @param softEdges Soft edge amounts (left, right, top, bottom) in 0-1 range
     * @param brightness Brightness multiplier (1.0 = normal)
     * @param gamma Gamma correction (1.0 = linear)
     * @param opacity Overall opacity (0-1)
     */
    void drawMappingSurface(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                            const DirectX::XMFLOAT2 corners[4],
                            const DirectX::XMFLOAT2 sourceUVs[4],
                            const DirectX::XMFLOAT4& softEdges,
                            float brightness,
                            float gamma,
                            float opacity);

    // ========================================================================
    // Offscreen Compose Target (for multi-clip compositing)
    // ========================================================================

    /**
     * Create the offscreen compose target at specified resolution.
     * @param width Target width in pixels
     * @param height Target height in pixels
     * @return true on success
     */
    bool createComposeTarget(uint32_t width, uint32_t height);

    /**
     * Begin rendering to the compose target (clears it first).
     * Call before drawing clips, end with endComposeTarget().
     */
    void beginComposeTarget();

    /**
     * End rendering to compose target and transition for sampling.
     */
    void endComposeTarget();

    /**
     * Get the compose target as ImGui texture ID for display.
     * @return ImTextureID for ImGui::Image, or nullptr if not ready
     */
    void* getComposeTargetTextureID() const;

    /**
     * Get compose target GPU handle for use with drawTexturedQuad.
     */
    D3D12_GPU_DESCRIPTOR_HANDLE getComposeTargetSrvHandle() const { return m_composeTargetSrvHandle; }

    /**
     * Get compose target dimensions.
     */
    uint32_t getComposeTargetWidth() const { return m_composeTargetWidth; }
    uint32_t getComposeTargetHeight() const { return m_composeTargetHeight; }

    /**
     * Check if compose target is ready.
     */
    bool isComposeTargetReady() const { return m_composeTargetReady; }

    // ========================================================================
    // Screenshot Capture
    // ========================================================================

    /**
     * Capture the compose target (video output) to a PNG file.
     * @param filepath Destination path for PNG file
     * @return true on success
     */
    bool captureComposeTargetToPNG(const std::string& filepath);

    /**
     * Capture the current back buffer (full window) to a PNG file.
     * @param filepath Destination path for PNG file
     * @return true on success
     */
    bool captureBackBufferToPNG(const std::string& filepath);

private:
    // Helper methods for initialization
    Result createDevice();
    Result createCommandQueue();
    Result createSwapChain(void* windowHandle, uint32_t width, uint32_t height);
    Result createRenderTargetViews();
    Result createCommandAllocators();
    Result createCommandList();
    Result createFence();
    void waitForGpu();
    void moveToNextFrame();

    // Helper methods for rendering pipeline
    Result compileShader(const std::wstring& filename, const char* entryPoint, const char* target, ID3DBlob** blob);
    Result createRootSignature();
    Result createPipelineState();
    Result createVertexBuffer();
    Result createConstantBuffer();
    void updateConstantBuffer(const DirectX::XMMATRIX& transform, const DirectX::XMFLOAT4& color, float opacity);

    // Helper methods for textured rendering pipeline
    Result createTexturedRootSignature();
    Result createTexturedPipelineState();

    // Helper methods for mapping surface rendering pipeline
    Result createMappingSurfaceRootSignature();
    Result createMappingSurfacePipelineState();
    Result createMappingSurfaceVertexBuffer();
    Result createMappingSurfaceConstantBuffer();

    // Helper methods for ImGui
    Result initializeImGui(GLFWwindow* window);
    void shutdownImGui();

    // Helper methods for screenshot capture
    bool ensureScreenshotStagingBuffer(uint32_t width, uint32_t height);
    bool readbackTextureToPNG(ID3D12Resource* sourceTexture,
                               D3D12_RESOURCE_STATES sourceState,
                               uint32_t width, uint32_t height,
                               const std::string& filepath);

private:
    static constexpr uint32_t FRAME_COUNT = 2; // Double buffering

    // D3D12 Core objects
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
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
    void* m_constantBufferData;  // Mapped pointer to constant buffer

    // ImGui objects
    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;  // Descriptor heap for ImGui fonts/textures

    // Video texture objects (legacy single-texture for backwards compatibility)
    ComPtr<ID3D12Resource> m_videoTexture;        // GPU texture for video frames
    ComPtr<ID3D12Resource> m_videoUploadBuffer;   // Upload heap for texture data
    D3D12_GPU_DESCRIPTOR_HANDLE m_videoTextureGpuHandle{};  // GPU handle for ImGui
    uint32_t m_videoTextureWidth{0};
    uint32_t m_videoTextureHeight{0};
    uint32_t m_srvDescriptorSize{0};

    // Multi-texture support (for multi-layer compositing)
    struct VideoTextureSlot {
        ComPtr<ID3D12Resource> texture;           // GPU texture resource
        ComPtr<ID3D12Resource> uploadBuffer;      // Upload heap buffer
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};  // GPU descriptor handle
        uint32_t width{0};
        uint32_t height{0};
        bool allocated{false};                    // Is this slot in use?
        bool firstUpload{true};                   // Track first upload for barrier state
    };
    VideoTextureSlot m_textureSlots[MAX_VIDEO_TEXTURE_SLOTS];

    // Textured rendering pipeline (for drawTexturedQuad)
    ComPtr<ID3D12RootSignature> m_texturedRootSignature;
    ComPtr<ID3D12PipelineState> m_texturedPipelineState;         // Normal blend
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateAdd;      // Additive blend
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateMultiply; // Multiply blend
    ComPtr<ID3D12PipelineState> m_texturedPipelineStateScreen;   // Screen blend

    // Mapping surface rendering pipeline (for projection mapping)
    ComPtr<ID3D12RootSignature> m_mappingSurfaceRootSignature;
    ComPtr<ID3D12PipelineState> m_mappingSurfacePipelineState;
    ComPtr<ID3D12Resource> m_mappingSurfaceVertexBuffer;
    ComPtr<ID3D12Resource> m_mappingSurfaceConstantBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_mappingSurfaceVertexBufferView;
    void* m_mappingSurfaceConstantBufferData{nullptr};

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

    // Offscreen compose target (for multi-clip compositing)
    ComPtr<ID3D12Resource> m_composeTarget;           // Render target texture
    ComPtr<ID3D12DescriptorHeap> m_composeTargetRtvHeap;  // RTV for rendering to it
    D3D12_GPU_DESCRIPTOR_HANDLE m_composeTargetSrvHandle{};  // SRV for sampling
    uint32_t m_composeTargetWidth{0};
    uint32_t m_composeTargetHeight{0};
    bool m_composeTargetReady{false};

    // State
    uint32_t m_currentBackBufferIndex;
    uint32_t m_rtvDescriptorSize;
    uint32_t m_width;
    uint32_t m_height;
    bool m_initialized;

    // Screenshot capture staging buffer
    ComPtr<ID3D12Resource> m_screenshotStagingBuffer;
    uint32_t m_screenshotStagingWidth{0};
    uint32_t m_screenshotStagingHeight{0};
    uint64_t m_screenshotStagingRowPitch{0};
};

} // namespace entity
