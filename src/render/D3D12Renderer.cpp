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

    // Unmap constant buffer
    if (m_constantBuffer && m_constantBufferData) {
        m_constantBuffer->Unmap(0, nullptr);
        m_constantBufferData = nullptr;
    }

    // Close fence event
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

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

    // Reset command allocator for current frame
    m_commandAllocators[m_currentBackBufferIndex]->Reset();

    // Reset command list
    m_commandList->Reset(m_commandAllocators[m_currentBackBufferIndex].Get(), nullptr);
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

    // Present
    hr = m_swapChain->Present(1, 0); // VSync on
    if (FAILED(hr)) {
        std::cerr << "Failed to present swap chain!" << std::endl;
    }

    // Move to next frame
    moveToNextFrame();
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
        }
        return Result::Failure;
    }

    return Result::Success;
}

Result D3D12Renderer::createRootSignature() {
    // Define root parameter for constant buffer
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0; // b0
    rootParameter.Descriptor.RegisterSpace = 0;
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
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_constantBuffer->Map(0, &readRange, &m_constantBufferData);
    if (FAILED(hr)) {
        std::cerr << "Failed to map constant buffer!" << std::endl;
        return Result::Failure;
    }

    return Result::Success;
}

void D3D12Renderer::updateConstantBuffer(const DirectX::XMMATRIX& transform, const DirectX::XMFLOAT4& color, float opacity) {
    LayerConstants constants;
    DirectX::XMStoreFloat4x4(&constants.transform, DirectX::XMMatrixTranspose(transform));
    constants.color = color;
    constants.opacity = opacity;
    constants.padding1 = 0.0f;
    constants.padding2 = 0.0f;
    constants.padding3 = 0.0f;

    memcpy(m_constantBufferData, &constants, sizeof(LayerConstants));
}

void D3D12Renderer::drawColoredQuad(const DirectX::XMMATRIX& transform, const DirectX::XMFLOAT4& color, float opacity) {
    if (!m_initialized) {
        return;
    }

    // Update constant buffer
    updateConstantBuffer(transform, color, opacity);

    // Set pipeline state
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set root constant buffer view
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

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

    // Set vertex buffer and draw
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->DrawInstanced(4, 1, 0, 0);
}

// ============================================================================
// ImGui Integration
// ============================================================================

Result D3D12Renderer::initializeImGui(GLFWwindow* window) {
    // Create descriptor heap for ImGui SRV (fonts/textures + video texture)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;  // One for font texture, one for video texture
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

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

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

    // Set ImGui descriptor heap
    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);

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
    static bool firstUpload = true;
    if (!firstUpload) {
        m_commandList->ResourceBarrier(1, &barrier);
    }
    firstUpload = false;

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

} // namespace entity
