/**
 * D3D12Renderer Implementation
 *
 * Basic D3D12 renderer that initializes the device and clears to a color.
 */

#include "entity/render/D3D12Renderer.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3dcompiler.h>
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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace entity {

D3D12Renderer::D3D12Renderer()
    : m_currentBackBufferIndex(0)
    , m_rtvDescriptorSize(0)
    , m_width(0)
    , m_height(0)
    , m_initialized(false)
    , m_fenceEvent(nullptr)
    , m_constantBufferData(nullptr)
{
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_fenceValues[i] = 0;
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

    // Create D3D12 device
    if (createDevice() != Result::Success) {
        std::cerr << "Failed to create D3D12 device!" << std::endl;
        return Result::Failure;
    }

    // Create command queue
    if (createCommandQueue() != Result::Success) {
        std::cerr << "Failed to create command queue!" << std::endl;
        return Result::Failure;
    }

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

    // Initialize ImGui
    if (initializeImGui(window) != Result::Success) {
        std::cerr << "Failed to initialize ImGui!" << std::endl;
        return Result::Failure;
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

    m_initialized = true;
    std::cout << "D3D12 renderer initialized successfully!" << std::endl;

    return Result::Success;
}

void D3D12Renderer::shutdown() {
    if (!m_initialized) {
        return;
    }

    std::cout << "Shutting down D3D12 renderer..." << std::endl;

    // Shutdown ImGui
    shutdownImGui();

    // Wait for GPU to finish all work
    waitForGpu();

    // Unmap constant buffers
    if (m_constantBuffer && m_constantBufferData) {
        m_constantBuffer->Unmap(0, nullptr);
        m_constantBufferData = nullptr;
    }

    if (m_mappingSurfaceConstantRing && m_mappingSurfaceConstantRingMapped) {
        m_mappingSurfaceConstantRing->Unmap(0, nullptr);
        m_mappingSurfaceConstantRingMapped = nullptr;
    }

    // Close fence event
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // Release multi-texture slots
    for (uint32_t i = 0; i < MAX_VIDEO_TEXTURE_SLOTS; ++i) {
        m_textureSlots[i].texture.Reset();
        m_textureSlots[i].uploadBuffer.Reset();
        m_textureSlots[i].allocated = false;
    }

    // Release legacy video upload buffer
    m_videoUploadBuffer.Reset();

    // Release compose target resources
    for (auto& target : m_composeTargets) {
        target.resource.Reset();
        target.rtvHeap.Reset();
    }
    m_composeTargets.clear();

    // Release textured pipeline objects
    m_texturedPipelineState.Reset();
    m_texturedRootSignature.Reset();

    // Release mapping surface pipeline objects
    m_mappingSurfacePipelineState.Reset();
    m_mappingSurfaceRootSignature.Reset();
    m_mappingSurfaceVertexBuffer.Reset();
    m_mappingSurfaceConstantRing.Reset();

    // Release rendering pipeline objects
    m_cbvHeap.Reset();
    m_constantBuffer.Reset();
    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();

    // Release all COM objects (ComPtr handles this automatically)
    m_commandList.Reset();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_commandAllocators[i].Reset();
        m_renderTargets[i].Reset();
    }
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_fence.Reset();
    m_device.Reset();

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

void D3D12Renderer::beginFrame() {
    // Get current back buffer index
    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    // CRIT-04: reset the per-frame mapping-surface CB ring cursor. Fence sync in
    // moveToNextFrame() has already ensured the GPU is done with this frame's region.
    m_mappingSurfaceDrawIndex = 0;
    m_mappingSurfaceOverflowed = false;

    // Reset command allocator for current frame
    m_commandAllocators[m_currentBackBufferIndex]->Reset();

    // Reset command list
    m_commandList->Reset(m_commandAllocators[m_currentBackBufferIndex].Get(), nullptr);

    // Reset descriptor heap cache (command list state is reset)
    m_currentDescriptorHeap = nullptr;

    // Set viewport and scissor rect to match back buffer size
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = {};
    scissorRect.right = static_cast<LONG>(m_width);
    scissorRect.bottom = static_cast<LONG>(m_height);

    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissorRect);
}

void D3D12Renderer::clear(float r, float g, float b, float a) {
    // Transition render target to RENDER_TARGET state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_renderTargets[m_currentBackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(1, &barrier);

    // Get RTV handle for current back buffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_currentBackBufferIndex * m_rtvDescriptorSize;

    // Set render target
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear the render target
    const float clearColor[] = { r, g, b, a };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
}

void D3D12Renderer::endFrame() {
    if (m_deviceLost) return;  // Don't pile more work onto a dead device.

    // Transition render target to PRESENT state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_renderTargets[m_currentBackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(1, &barrier);

    // Close command list
    HRESULT hr = m_commandList->Close();
    if (FAILED(hr)) {
        std::cerr << "Failed to close command list!" << std::endl;
        return;
    }

    // Execute command list
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    // Present — failures here commonly signal device-removed on live systems
    // (driver timeout, TDR, unplugged external GPU). We detect and latch so the
    // Engine can shut down cleanly instead of spinning on a dead device.
    hr = m_swapChain->Present(1, 0); // VSync on
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            handleDeviceLost(hr, "Present");
            return;
        }
        std::cerr << "Failed to present swap chain! HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
    }

    // Move to next frame
    moveToNextFrame();
}

void D3D12Renderer::handleDeviceLost(HRESULT hr, const char* site) {
    if (m_deviceLost) return;  // Already reported.
    m_deviceLost = true;

    HRESULT removedReason = m_device ? m_device->GetDeviceRemovedReason() : hr;
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
}

Result D3D12Renderer::createDevice() {
    // Enable debug layer in debug builds
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        std::cout << "D3D12 debug layer enabled" << std::endl;
    }
#endif

    // Create device
    HRESULT hr = D3D12CreateDevice(
        nullptr,                    // Use default adapter
        D3D_FEATURE_LEVEL_11_0,     // Minimum feature level
        IID_PPV_ARGS(&m_device)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D12 device! HRESULT: 0x" << std::hex << hr << std::endl;
        return Result::Failure;
    }

    std::cout << "D3D12 device created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        std::cerr << "Failed to create command queue!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Command queue created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createSwapChain(void* windowHandle, uint32_t width, uint32_t height) {
    // Create DXGI factory
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXGI factory!" << std::endl;
        return Result::Failure;
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
    hr = factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
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

    HRESULT hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create RTV descriptor heap!" << std::endl;
        return Result::Failure;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create RTVs for each frame
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            std::cerr << "Failed to get swap chain buffer " << i << "!" << std::endl;
            return Result::Failure;
        }

        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    std::cout << "Render target views created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createCommandAllocators() {
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        HRESULT hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create command allocator " << i << "!" << std::endl;
            return Result::Failure;
        }
    }

    std::cout << "Command allocators created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createCommandList() {
    HRESULT hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create command list!" << std::endl;
        return Result::Failure;
    }

    // Close command list (it starts in recording state)
    m_commandList->Close();

    std::cout << "Command list created" << std::endl;
    return Result::Success;
}

Result D3D12Renderer::createFence() {
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        std::cerr << "Failed to create fence!" << std::endl;
        return Result::Failure;
    }

    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_fenceValues[i] = 0;
    }

    // Create fence event
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        std::cerr << "Failed to create fence event!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Fence created" << std::endl;
    return Result::Success;
}

void D3D12Renderer::waitForGpu() {
    // Schedule a signal command in the queue
    // Use a fence value higher than any frame's current value to ensure all work completes
    uint64_t maxFenceValue = 0;
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (m_fenceValues[i] > maxFenceValue) {
            maxFenceValue = m_fenceValues[i];
        }
    }

    const uint64_t fenceValueToWaitFor = maxFenceValue;
    m_commandQueue->Signal(m_fence.Get(), fenceValueToWaitFor);

    // Wait until the GPU has completed all work up to this fence point
    if (m_fence->GetCompletedValue() < fenceValueToWaitFor) {
        m_fence->SetEventOnCompletion(fenceValueToWaitFor, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // Update all fence values to be at least this value since we know all work is done
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        m_fenceValues[i] = fenceValueToWaitFor + 1;
    }
}

void D3D12Renderer::moveToNextFrame() {
    // Signal and increment fence value for current frame
    const uint64_t currentFenceValue = m_fenceValues[m_currentBackBufferIndex];
    m_commandQueue->Signal(m_fence.Get(), currentFenceValue);

    // Move to next frame
    m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait if next frame is not ready yet
    if (m_fence->GetCompletedValue() < m_fenceValues[m_currentBackBufferIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_currentBackBufferIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // Increment fence value for next frame
    m_fenceValues[m_currentBackBufferIndex] = currentFenceValue + 1;
}

// ============================================================================
// Rendering Pipeline Methods
// ============================================================================

Result D3D12Renderer::compileShader(const std::wstring& filename, const char* entryPoint, const char* target, ID3DBlob** blob) {
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(
        filename.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint,
        target,
        compileFlags,
        0,
        blob,
        &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Shader compilation failed: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        } else {
            std::wcerr << L"Shader file not found or inaccessible: " << filename << std::endl;
        }
        return Result::Failure;
    }

    return Result::Success;
}

Result D3D12Renderer::createRootSignature() {
    // Define root parameter using 32-bit constants (not CBV!)
    // This ensures each draw call gets its own constant values, because
    // SetGraphicsRoot32BitConstants copies data into the command buffer
    // at record time, unlike CBV which just stores a pointer.
    //
    // LayerConstants = 24 floats = 96 bytes (transform 16 + color 4 + opacity+padding 4)
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameter.Constants.ShaderRegister = 0; // b0
    rootParameter.Constants.RegisterSpace = 0;
    rootParameter.Constants.Num32BitValues = 24; // sizeof(LayerConstants) / 4
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
    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
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

    if (compileShader(L"shaders/composite_vs.hlsl", "VSMain", "vs_5_1", &vertexShader) != Result::Success) {
        std::cerr << "Failed to compile vertex shader!" << std::endl;
        return Result::Failure;
    }

    if (compileShader(L"shaders/solid_color_ps.hlsl", "PSMain", "ps_5_1", &pixelShader) != Result::Success) {
        std::cerr << "Failed to compile pixel shader!" << std::endl;
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
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

    HRESULT hr = m_device->CreateCommittedResource(
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

    HRESULT hr = m_device->CreateCommittedResource(
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
    constants.padding2 = 0.0f;
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
    constants.padding2 = 0.0f;
    constants.padding3 = 0.0f;

    // Set pipeline state
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set root constants (copies data into command buffer - each draw gets its own values!)
    m_commandList->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);

    // NOTE: Don't override viewport/scissor - caller (beginComposeTarget or beginFrame) sets these
    // This allows drawColoredQuad to work correctly with both main window and offscreen targets

    // Set vertex buffer and draw
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->DrawInstanced(4, 1, 0, 0);
}

// ============================================================================
// ImGui Integration
// ============================================================================

Result D3D12Renderer::initializeImGui(GLFWwindow* window) {
    // Create descriptor heap for ImGui SRV (fonts/textures + video textures)
    // Descriptor heap layout:
    // Slot 0: ImGui font texture
    // Slot 1: Legacy single video texture (for backwards compatibility)
    // Slots 2 to 2+MAX_COMPOSE_TARGETS-1: Compose targets (one per screen)
    // Slots 2+MAX_COMPOSE_TARGETS to end: Video texture slots for compositing layers
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // Heap layout: [0]=font, [1]=legacy, [2..2+MAX_COMPOSE_TARGETS-1]=compose targets, [2+MAX_COMPOSE_TARGETS..]=video textures
    heapDesc.NumDescriptors = 2 + MAX_COMPOSE_TARGETS + MAX_VIDEO_TEXTURE_SLOTS;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_imguiSrvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create ImGui descriptor heap!" << std::endl;
        return Result::Failure;
    }

    // Store descriptor size for later use
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
        m_device.Get(),
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
    ImGui::Render();

    // Set ImGui descriptor heap (only if not already set)
    if (m_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    // Render ImGui draw data
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
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

        hr = m_device->CreateCommittedResource(
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

        // Create upload buffer
        UINT64 uploadBufferSize = 0;
        m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

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

        hr = m_device->CreateCommittedResource(
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

        // Create SRV for the texture (at descriptor index 1, after ImGui font)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += m_srvDescriptorSize;  // Offset to slot 1

        m_device->CreateShaderResourceView(m_videoTexture.Get(), &srvDesc, cpuHandle);

        // Store GPU handle for ImGui
        m_videoTextureGpuHandle = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        m_videoTextureGpuHandle.ptr += m_srvDescriptorSize;  // Offset to slot 1

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

    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

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
        m_commandList->ResourceBarrier(1, &barrier);
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

    m_commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    // Transition texture back to shader resource state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &barrier);

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
    for (uint32_t i = 0; i < MAX_VIDEO_TEXTURE_SLOTS; ++i) {
        if (!m_textureSlots[i].allocated) {
            m_textureSlots[i].allocated = true;
            m_textureSlots[i].firstUpload = true;
            m_textureSlots[i].width = 0;
            m_textureSlots[i].height = 0;
            std::cout << "Allocated video texture slot " << i << std::endl;
            return i;
        }
    }
    std::cerr << "No available video texture slots!" << std::endl;
    return UINT32_MAX;
}

void D3D12Renderer::freeVideoTextureSlot(uint32_t slot) {
    if (slot >= MAX_VIDEO_TEXTURE_SLOTS) {
        return;
    }

    // Wait for GPU before releasing resources
    waitForGpu();

    m_textureSlots[slot].texture.Reset();
    m_textureSlots[slot].uploadBuffer.Reset();
    m_textureSlots[slot].gpuHandle = {};
    m_textureSlots[slot].width = 0;
    m_textureSlots[slot].height = 0;
    m_textureSlots[slot].allocated = false;
    m_textureSlots[slot].firstUpload = true;

    std::cout << "Freed video texture slot " << slot << std::endl;
}

bool D3D12Renderer::uploadVideoFrameToSlot(uint32_t slot,
                                           const uint8_t* rgbaData,
                                           uint32_t width, uint32_t height,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* outSrvHandle) {
    if (slot >= MAX_VIDEO_TEXTURE_SLOTS) {
        std::cerr << "Texture slot " << slot << " out of bounds (max " << MAX_VIDEO_TEXTURE_SLOTS << ")" << std::endl;
        return false;
    }

    if (!m_initialized || !m_textureSlots[slot].allocated) {
        return false;
    }

    if (!rgbaData || width == 0 || height == 0) {
        return false;
    }

    VideoTextureSlot& texSlot = m_textureSlots[slot];
    HRESULT hr;

    // Check if we need to recreate the texture (size changed)
    if (texSlot.texture && (texSlot.width != width || texSlot.height != height)) {
        waitForGpu();
        texSlot.texture.Reset();
        texSlot.uploadBuffer.Reset();
        texSlot.width = 0;
        texSlot.height = 0;
        texSlot.firstUpload = true;
    }

    // Create texture if it doesn't exist
    if (!texSlot.texture) {
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

        hr = m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texSlot.texture)
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create texture for slot " << slot << "!" << std::endl;
            return false;
        }

        // Create upload buffer
        UINT64 uploadBufferSize = 0;
        m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

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

        hr = m_device->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&texSlot.uploadBuffer)
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create upload buffer for slot " << slot << "!" << std::endl;
            texSlot.texture.Reset();
            return false;
        }

        // Create SRV for the texture
        // Slots are at indices 3+ (0=font, 1=legacy, 2=compose target)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        // Video textures start after compose targets: [2 + MAX_COMPOSE_TARGETS + slot]
        uint32_t descriptorIndex = 2 + MAX_COMPOSE_TARGETS + slot;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += descriptorIndex * m_srvDescriptorSize;

        m_device->CreateShaderResourceView(texSlot.texture.Get(), &srvDesc, cpuHandle);

        // Store GPU handle
        texSlot.gpuHandle = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        texSlot.gpuHandle.ptr += descriptorIndex * m_srvDescriptorSize;

        texSlot.width = width;
        texSlot.height = height;

        std::cout << "Created texture for slot " << slot << ": " << width << "x" << height << std::endl;
    }

    // Upload texture data
    D3D12_RESOURCE_DESC textureDesc = texSlot.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSizeInBytes;
    UINT64 totalBytes;

    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    // Map upload buffer and copy data
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = texSlot.uploadBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        std::cerr << "Failed to map upload buffer for slot " << slot << "!" << std::endl;
        return false;
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

    texSlot.uploadBuffer->Unmap(0, nullptr);

    // Record copy command
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texSlot.texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Only transition if texture was previously used
    if (!texSlot.firstUpload) {
        m_commandList->ResourceBarrier(1, &barrier);
    }
    texSlot.firstUpload = false;

    // Copy from upload buffer to texture
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = texSlot.uploadBuffer.Get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = texSlot.texture.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    m_commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    // Transition texture back to shader resource state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &barrier);

    if (outSrvHandle) {
        *outSrvHandle = texSlot.gpuHandle;
    }

    return true;
}

// ============================================================================
// Textured Rendering Pipeline
// ============================================================================

Result D3D12Renderer::createTexturedRootSignature() {
    // Root parameters:
    // [0] Root constants for LayerConstants (b0) - 24 floats = 96 bytes
    // [1] Descriptor table for texture SRV (t0)
    // Static sampler for texture sampling

    D3D12_ROOT_PARAMETER rootParameters[2] = {};

    // Parameter 0: Root constants (not CBV - ensures each draw gets its own values)
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[0].Constants.ShaderRegister = 0; // b0
    rootParameters[0].Constants.RegisterSpace = 0;
    rootParameters[0].Constants.Num32BitValues = 24; // sizeof(LayerConstants) / 4
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
    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_texturedRootSignature));
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

    if (compileShader(L"shaders/composite_vs.hlsl", "VSMain", "vs_5_1", &vertexShader) != Result::Success) {
        std::cerr << "Failed to compile textured vertex shader!" << std::endl;
        return Result::Failure;
    }

    if (compileShader(L"shaders/composite_ps.hlsl", "PSMain", "ps_5_1", &pixelShader) != Result::Success) {
        std::cerr << "Failed to compile textured pixel shader!" << std::endl;
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineState));
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
    hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateAdd));
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
    hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateMultiply));
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
    hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_texturedPipelineStateScreen));
    if (FAILED(hr)) {
        std::cerr << "Failed to create Screen blend pipeline state!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Textured pipeline states created (Normal, Add, Multiply, Screen)" << std::endl;
    return Result::Success;
}

void D3D12Renderer::drawTexturedQuad(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
                                     const DirectX::XMMATRIX& transform,
                                     float opacity,
                                     BlendMode blendMode) {
    if (!m_initialized || textureSrv.ptr == 0) {
        return;
    }

    // Build constants structure with white color (texture provides the color)
    LayerConstants constants;
    DirectX::XMStoreFloat4x4(&constants.transform, DirectX::XMMatrixTranspose(transform));
    constants.color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constants.opacity = opacity;
    constants.blendMode = static_cast<uint32_t>(blendMode);  // Pass blend mode to shader
    constants.padding2 = 0.0f;
    constants.padding3 = 0.0f;

    // Select pipeline state based on blend mode
    ID3D12PipelineState* pipelineState = m_texturedPipelineState.Get();  // Default: Normal
    switch (blendMode) {
        case BlendMode::Add:
            pipelineState = m_texturedPipelineStateAdd.Get();
            break;
        case BlendMode::Multiply:
            pipelineState = m_texturedPipelineStateMultiply.Get();
            break;
        case BlendMode::Screen:
            pipelineState = m_texturedPipelineStateScreen.Get();
            break;
        default:
            // For unsupported blend modes, fall back to Normal
            pipelineState = m_texturedPipelineState.Get();
            break;
    }

    // Set textured pipeline state
    m_commandList->SetPipelineState(pipelineState);
    m_commandList->SetGraphicsRootSignature(m_texturedRootSignature.Get());

    // Set descriptor heap (only if not already set)
    if (m_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    // Set root parameters - use root constants (copied into command buffer)
    m_commandList->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);
    m_commandList->SetGraphicsRootDescriptorTable(1, textureSrv);

    // NOTE: Don't override viewport/scissor - caller (beginComposeTarget or beginFrame) sets these
    // This allows drawTexturedQuad to work correctly with both main window and offscreen targets

    // Set vertex buffer and draw
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->DrawInstanced(4, 1, 0, 0);
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
    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_mappingSurfaceRootSignature));
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

    if (compileShader(L"shaders/mapping_surface_vs.hlsl", "VSMain", "vs_5_1", &vertexShader) != Result::Success) {
        std::cerr << "Failed to compile mapping surface vertex shader!" << std::endl;
        return Result::Failure;
    }

    if (compileShader(L"shaders/mapping_surface_ps.hlsl", "PSMain", "ps_5_1", &pixelShader) != Result::Success) {
        std::cerr << "Failed to compile mapping surface pixel shader!" << std::endl;
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

    HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_mappingSurfacePipelineState));
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

    HRESULT hr = m_device->CreateCommittedResource(
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
    // own 256-aligned slot. beginFrame() resets the draw index; moveToNextFrame()
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

    HRESULT hr = m_device->CreateCommittedResource(
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

void D3D12Renderer::drawMappingSurface(D3D12_GPU_DESCRIPTOR_HANDLE textureSrv,
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
    // single frame, each drawMappingSurface() call gets its own CB region so the
    // GPU reads consistent data at execute time even with multiple surfaces. Across
    // frames, the fence in moveToNextFrame() guarantees the CPU doesn't overwrite a
    // slot the GPU is still reading.
    if (m_mappingSurfaceDrawIndex >= MAX_MAPPING_SURFACES_PER_FRAME) {
        if (!m_mappingSurfaceOverflowed) {
            std::cerr << "[drawMappingSurface] Surface limit ("
                      << MAX_MAPPING_SURFACES_PER_FRAME
                      << "/frame) exceeded — additional surfaces dropped this frame" << std::endl;
            m_mappingSurfaceOverflowed = true;
        }
        return;
    }

    const uint32_t frameBase = m_currentBackBufferIndex * MAX_MAPPING_SURFACES_PER_FRAME;
    const uint32_t slotOffset = (frameBase + m_mappingSurfaceDrawIndex) * m_mappingSurfaceSlotSize;

    MappingSurfaceConstants constants;
    for (int i = 0; i < 4; ++i) {
        constants.corners[i] = DirectX::XMFLOAT4(corners[i].x, corners[i].y, 0.0f, 0.0f);
        constants.sourceUVs[i] = DirectX::XMFLOAT4(sourceUVs[i].x, sourceUVs[i].y, 0.0f, 0.0f);
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

    // Set mapping surface pipeline state
    m_commandList->SetPipelineState(m_mappingSurfacePipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_mappingSurfaceRootSignature.Get());

    // Set descriptor heap (only if not already set)
    if (m_currentDescriptorHeap != m_imguiSrvHeap.Get()) {
        ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_currentDescriptorHeap = m_imguiSrvHeap.Get();
    }

    // Set root parameters — bind this draw's unique slot in the ring buffer
    const D3D12_GPU_VIRTUAL_ADDRESS slotGpuVa =
        m_mappingSurfaceConstantRing->GetGPUVirtualAddress() + slotOffset;
    m_commandList->SetGraphicsRootConstantBufferView(0, slotGpuVa);
    m_commandList->SetGraphicsRootDescriptorTable(1, textureSrv);

    m_mappingSurfaceDrawIndex++;

    // Set viewport and scissor rect
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = {};
    scissorRect.right = m_width;
    scissorRect.bottom = m_height;

    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissorRect);

    // Set vertex buffer and draw (6 vertices = 2 triangles)
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_mappingSurfaceVertexBufferView);
    m_commandList->DrawInstanced(6, 1, 0, 0);
}

// ========================================================================
// Compose Target (Offscreen Render Target for Multi-Clip Compositing)
// ========================================================================

uint32_t D3D12Renderer::createComposeTarget(uint32_t width, uint32_t height) {
    if (!m_initialized || !m_device) {
        std::cerr << "Cannot create compose target: renderer not initialized" << std::endl;
        return UINT32_MAX;
    }

    if (width == 0 || height == 0) {
        std::cerr << "Invalid compose target dimensions: " << width << "x" << height << std::endl;
        return UINT32_MAX;
    }

    // Wait for GPU before modifying resources
    waitForGpu();

    // Create new compose target
    uint32_t slot = static_cast<uint32_t>(m_composeTargets.size());
    m_composeTargets.emplace_back();
    ComposeTarget& target = m_composeTargets.back();

    std::cout << "Creating compose target " << slot << ": " << width << "x" << height << std::endl;

    // Create RTV descriptor heap for compose target
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&target.rtvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create compose target RTV heap!" << std::endl;
        m_composeTargets.pop_back();
        return UINT32_MAX;
    }

    // Create the render target texture
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
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // Start in shader resource state
        &clearValue,
        IID_PPV_ARGS(&target.resource)
    );

    if (FAILED(hr)) {
        std::cerr << "Failed to create compose target texture!" << std::endl;
        m_composeTargets.pop_back();
        return UINT32_MAX;
    }

    // Create RTV for the compose target
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = target.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(target.resource.Get(), &rtvDesc, rtvHandle);

    // Create SRV for the compose target in the ImGui heap
    // Heap layout: 0=fonts, 1=legacy, 2+=compose targets (one per screen)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    // Compose targets start at slot 2 (0=fonts, 1=legacy)
    uint32_t heapSlot = 2 + slot;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += m_srvDescriptorSize * heapSlot;

    m_device->CreateShaderResourceView(target.resource.Get(), &srvDesc, cpuHandle);

    // Store GPU handle
    target.srvHandle = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    target.srvHandle.ptr += m_srvDescriptorSize * heapSlot;

    target.width = width;
    target.height = height;
    target.ready = true;

    std::cout << "Compose target " << slot << " created successfully: " << width << "x" << height << std::endl;
    std::cout << "  Heap slot: " << heapSlot << std::endl;
    std::cout << "  SRV descriptor size: " << m_srvDescriptorSize << std::endl;
    std::cout << "  GPU handle ptr: " << target.srvHandle.ptr << std::endl;
    std::cout << "  Heap start ptr: " << m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr << std::endl;

    return slot;
}

void D3D12Renderer::beginComposeTarget(uint32_t slot) {
    if (slot >= m_composeTargets.size() || !m_composeTargets[slot].ready) {
        return;
    }

    ComposeTarget& target = m_composeTargets[slot];
    m_currentComposeTargetSlot = slot;

    // Transition compose target from shader resource to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Get RTV handle
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = target.rtvHeap->GetCPUDescriptorHandleForHeapStart();

    // Set compose target as render target
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear to black (compose target background)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Set viewport and scissor for compose target
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(target.width);
    viewport.Height = static_cast<float>(target.height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = {};
    scissorRect.right = target.width;
    scissorRect.bottom = target.height;

    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissorRect);
}

void D3D12Renderer::endComposeTarget() {
    if (m_currentComposeTargetSlot >= m_composeTargets.size() || !m_composeTargets[m_currentComposeTargetSlot].ready) {
        return;
    }

    ComposeTarget& target = m_composeTargets[m_currentComposeTargetSlot];

    // Transition compose target from render target to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Restore the main render target (back buffer)
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_currentBackBufferIndex * m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Restore viewport for main render target
    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissorRect = {};
    scissorRect.right = m_width;
    scissorRect.bottom = m_height;

    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissorRect);
}

void* D3D12Renderer::getComposeTargetTextureID(uint32_t slot) const {
    if (slot < m_composeTargets.size() && m_composeTargets[slot].ready && m_composeTargets[slot].srvHandle.ptr != 0) {
        return reinterpret_cast<void*>(m_composeTargets[slot].srvHandle.ptr);
    }
    return nullptr;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::getComposeTargetSrvHandle(uint32_t slot) const {
    if (slot < m_composeTargets.size() && m_composeTargets[slot].ready) {
        return m_composeTargets[slot].srvHandle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
    return nullHandle;
}

uint32_t D3D12Renderer::getComposeTargetWidth(uint32_t slot) const {
    if (slot < m_composeTargets.size()) {
        return m_composeTargets[slot].width;
    }
    return 0;
}

uint32_t D3D12Renderer::getComposeTargetHeight(uint32_t slot) const {
    if (slot < m_composeTargets.size()) {
        return m_composeTargets[slot].height;
    }
    return 0;
}

bool D3D12Renderer::isComposeTargetReady(uint32_t slot) const {
    return (slot < m_composeTargets.size() && m_composeTargets[slot].ready);
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

    HRESULT hr = m_device->CreateCommittedResource(
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
    HRESULT hr = m_commandAllocators[m_currentBackBufferIndex]->Reset();
    if (FAILED(hr)) {
        std::cerr << "[Readback] Failed to reset command allocator!" << std::endl;
        return false;
    }

    hr = m_commandList->Reset(m_commandAllocators[m_currentBackBufferIndex].Get(), nullptr);
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
        m_commandList->ResourceBarrier(1, &barrier);
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
    m_commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    // Transition source texture back to original state
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = sourceTexture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = sourceState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    // Close and execute command list
    hr = m_commandList->Close();
    if (FAILED(hr)) {
        std::cerr << "[Readback] Failed to close command list!" << std::endl;
        return false;
    }

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

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
    if (slot >= m_composeTargets.size() || !m_composeTargets[slot].ready) {
        std::cerr << "[Screenshot] Compose target " << slot << " not ready!" << std::endl;
        return false;
    }

    const ComposeTarget& target = m_composeTargets[slot];
    return readbackTextureToPNG(
        target.resource.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        target.width,
        target.height,
        filepath
    );
}

bool D3D12Renderer::readComposeTargetPixels(uint32_t slot,
                                             uint32_t& outWidth,
                                             uint32_t& outHeight,
                                             std::vector<uint8_t>& outPixels) {
    if (slot >= m_composeTargets.size() || !m_composeTargets[slot].ready) {
        std::cerr << "[Readback] Compose target " << slot << " not ready!" << std::endl;
        return false;
    }

    const ComposeTarget& target = m_composeTargets[slot];
    outWidth = target.width;
    outHeight = target.height;
    return readbackTextureToPixels(
        target.resource.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        target.width,
        target.height,
        outPixels
    );
}

bool D3D12Renderer::captureBackBufferToPNG(const std::string& filepath) {
    if (!m_initialized) {
        std::cerr << "[Screenshot] Renderer not initialized!" << std::endl;
        return false;
    }

    // Note: Back buffer should be in PRESENT state after endFrame()
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
            if (tex.slot < MAX_VIDEO_TEXTURE_SLOTS && m_textureSlots[tex.slot].allocated) {
                return m_textureSlots[tex.slot].gpuHandle;
            }
            break;
        case TextureRef::Kind::ComposeTarget:
            if (tex.slot < m_composeTargets.size() && m_composeTargets[tex.slot].ready) {
                return m_composeTargets[tex.slot].srvHandle;
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
    return uploadVideoFrameToSlot(slot, rgba, width, height, &discard);
}

TextureRef D3D12Renderer::getVideoTexture(uint32_t slot) const {
    if (slot < MAX_VIDEO_TEXTURE_SLOTS && m_textureSlots[slot].allocated) {
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
                                      BlendMode blendMode) {
    const D3D12_GPU_DESCRIPTOR_HANDLE srv = resolveTextureHandle(texture);
    if (srv.ptr == 0) return;  // Invalid or unready texture — drop silently
    drawTexturedQuad(srv, glmToXm(transform), opacity, blendMode);
}

void D3D12Renderer::drawMappingSurface(TextureRef texture,
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

    drawMappingSurface(srv, xmCorners, xmUVs, xmSoft, brightness, gamma, opacity);
}

} // namespace entity
