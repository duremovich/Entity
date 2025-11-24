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

    // Helper methods for ImGui
    Result initializeImGui(GLFWwindow* window);
    void shutdownImGui();

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

    // Video texture objects
    ComPtr<ID3D12Resource> m_videoTexture;        // GPU texture for video frames
    ComPtr<ID3D12Resource> m_videoUploadBuffer;   // Upload heap for texture data
    D3D12_GPU_DESCRIPTOR_HANDLE m_videoTextureGpuHandle{};  // GPU handle for ImGui
    uint32_t m_videoTextureWidth{0};
    uint32_t m_videoTextureHeight{0};
    uint32_t m_srvDescriptorSize{0};

    // Constant buffer structure (must match HLSL)
    struct LayerConstants {
        DirectX::XMFLOAT4X4 transform;
        DirectX::XMFLOAT4 color;
        float opacity;
        float padding1;
        float padding2;
        float padding3;
    };

    // State
    uint32_t m_currentBackBufferIndex;
    uint32_t m_rtvDescriptorSize;
    uint32_t m_width;
    uint32_t m_height;
    bool m_initialized;
};

} // namespace entity
