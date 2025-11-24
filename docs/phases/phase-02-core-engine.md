# Phase 2: Core Engine & Window

**Status**: NOT STARTED
**Expected Start**: After Phase 1 validation complete
**Estimated Duration**: 1-2 weeks

---

## Overview

Phase 2 establishes the core engine infrastructure, including the main window, D3D12 rendering foundation, and the EnTT-based Entity Component System. This phase creates the fundamental runtime environment upon which all other systems will be built.

---

## Goals

- Initialize GLFW window with proper D3D12 configuration
- Set up D3D12 device, command queue, and swap chain
- Create basic EnTT registry for entity management
- Implement main render loop with fixed timestep
- Get a window displaying a clear color (proof of life)

---

## Tasks

### 1. Core Engine Class

**Files to Create**:
- `include/entity/core/Engine.hpp`
- `src/core/Engine.cpp`

**Responsibilities**:
- Manage the EnTT registry instance
- Implement update loop with fixed timestep
- Handle initialization and shutdown sequence
- Coordinate systems execution order

**Key Implementation Details**:

```cpp
// Engine.hpp
#pragma once
#include <entt/entt.hpp>
#include <memory>

namespace entity::core {

class Engine {
public:
    Engine();
    ~Engine();

    // Initialize engine and all subsystems
    bool initialize();

    // Main update loop (call each frame)
    void update(float deltaTime);

    // Clean shutdown
    void shutdown();

    // Access to ECS registry
    entt::registry& getRegistry() { return m_registry; }

private:
    entt::registry m_registry;

    // Fixed timestep accumulator
    float m_timeAccumulator;
    const float m_fixedTimestep = 1.0f / 60.0f; // 60 FPS
};

} // namespace entity::core
```

**Validation**:
- [ ] Engine class compiles
- [ ] Can create Engine instance
- [ ] initialize() returns true
- [ ] update() can be called repeatedly
- [ ] shutdown() cleans up without crashes

---

### 2. Types and Enums

**Files to Create**:
- `include/entity/core/Types.hpp`

**Contents**:
- Common type aliases
- Core enumerations used across systems
- Forward declarations

**Required Enums**:

```cpp
// Types.hpp
#pragma once
#include <cstdint>

namespace entity::core {

// Media types supported by the engine
enum class MediaType {
    Unknown,
    Video,          // Standard video codecs (ProRes, HAP, etc.)
    PNGSequence,    // Image sequence
    AudioTrack      // Future: audio support
};

// Blend modes for layer compositing
enum class BlendMode {
    Normal,         // Standard alpha blend
    Additive,       // Add source to destination
    Multiply,       // Multiply source with destination
    Screen,         // Screen blend mode
    Overlay         // Overlay blend mode
    // More modes added in Phase 10
};

// Transport state for playback control
enum class TransportState {
    Stopped,        // Not playing, at position 0
    Playing,        // Actively playing
    Paused          // Paused at current position
};

// Type aliases
using EntityID = entt::entity;
using FrameNumber = int64_t;
using TimeStamp = double; // In seconds

} // namespace entity::core
```

**Validation**:
- [ ] Types.hpp compiles
- [ ] Enums can be used in switch statements
- [ ] Type aliases resolve correctly

---

### 3. GLFW Window

**Files to Create**:
- `include/entity/core/Window.hpp`
- `src/core/Window.cpp`

**Responsibilities**:
- Create GLFW window with GLFW_NO_API hint (for D3D12)
- Get Win32 HWND handle for D3D12 swap chain creation
- Handle input events (keyboard, mouse)
- Handle window resize events

**Key Implementation Details**:

```cpp
// Window.hpp
#pragma once
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <string>

namespace entity::core {

class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    // Initialize GLFW and create window
    bool create();

    // Poll events and swap buffers
    void update();

    // Check if window should close
    bool shouldClose() const;

    // Get Win32 HWND for D3D12
    HWND getHWND() const;

    // Get window dimensions
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

private:
    GLFWwindow* m_window;
    int m_width;
    int m_height;
    std::string m_title;

    // GLFW callbacks
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
};

} // namespace entity::core
```

**GLFW Initialization**:
```cpp
glfwInit();
glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // No OpenGL/Vulkan
m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
HWND hwnd = glfwGetWin32Window(m_window);
```

**Validation**:
- [ ] Window opens and is visible
- [ ] HWND is valid (non-null)
- [ ] Window can be resized
- [ ] Input events are received
- [ ] Window closes cleanly

---

### 4. D3D12 Initialization

**Files to Create**:
- `include/entity/render/D3D12Renderer.hpp`
- `src/render/D3D12Renderer.cpp`
- `src/render/D3D12Device.cpp`

**Responsibilities**:
- Create D3D12 device and command queue
- Create DXGI swap chain
- Set up render target views (RTV)
- Create descriptor heaps (RTV, SRV/CBV/UAV)
- Enable debug layer in debug builds

**Key Implementation Details**:

**Device Creation**:
```cpp
// Enable debug layer (debug builds only)
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif

// Create device
ComPtr<ID3D12Device> device;
D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
```

**Command Queue**:
```cpp
D3D12_COMMAND_QUEUE_DESC queueDesc = {};
queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
```

**Swap Chain**:
```cpp
DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
swapChainDesc.Width = width;
swapChainDesc.Height = height;
swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
swapChainDesc.BufferCount = 2; // Double buffering
swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
swapChainDesc.SampleDesc.Count = 1;

ComPtr<IDXGIFactory4> factory;
CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

ComPtr<IDXGISwapChain1> swapChain1;
factory->CreateSwapChainForHwnd(
    commandQueue.Get(),
    hwnd,
    &swapChainDesc,
    nullptr,
    nullptr,
    &swapChain1
);
swapChain1.As(&swapChain); // Convert to IDXGISwapChain3
```

**Descriptor Heaps**:
```cpp
// RTV descriptor heap
D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
rtvHeapDesc.NumDescriptors = 2; // One per swap chain buffer
rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));

// SRV/CBV/UAV descriptor heap (for textures, constant buffers, etc.)
D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
srvHeapDesc.NumDescriptors = 100; // Adjust as needed
srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));
```

**Render Target Views**:
```cpp
CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
for (UINT i = 0; i < 2; ++i) {
    swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i]));
    device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, rtvDescriptorSize);
}
```

**Clear Screen (Proof of Life)**:
```cpp
void D3D12Renderer::render() {
    // Get current back buffer
    UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();

    // Transition to render target state
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        renderTargets[backBufferIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );
    commandList->ResourceBarrier(1, &barrier);

    // Clear to cornflower blue (0.4f, 0.6f, 0.9f, 1.0f)
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                              backBufferIndex, rtvDescriptorSize);
    const float clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Transition back to present state
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        renderTargets[backBufferIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT
    );
    commandList->ResourceBarrier(1, &barrier);

    // Execute command list
    commandList->Close();
    ID3D12CommandList* lists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, lists);

    // Present
    swapChain->Present(1, 0); // VSync enabled

    // Wait for GPU (simple synchronization for now)
    // TODO: Implement proper fence-based synchronization
}
```

**Validation**:
- [ ] D3D12 device created successfully
- [ ] Command queue created
- [ ] Swap chain created for window HWND
- [ ] Descriptor heaps allocated
- [ ] RTVs created for swap chain buffers
- [ ] Debug layer enabled (debug builds)
- [ ] No D3D12 validation errors in output

---

### 5. Main Application

**Files to Create/Modify**:
- `apps/editor/main.cpp`

**Responsibilities**:
- Initialize Engine
- Create Window
- Initialize D3D12Renderer
- Run main loop
- Handle clean shutdown

**Main Loop Structure**:

```cpp
#include "entity/core/Engine.hpp"
#include "entity/core/Window.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include <chrono>

int main() {
    // Create window
    entity::core::Window window(1920, 1080, "Entity Media Server");
    if (!window.create()) {
        return -1;
    }

    // Create engine
    entity::core::Engine engine;
    if (!engine.initialize()) {
        return -1;
    }

    // Create renderer
    entity::render::D3D12Renderer renderer;
    if (!renderer.initialize(window.getHWND(), window.getWidth(), window.getHeight())) {
        return -1;
    }

    // Main loop
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (!window.shouldClose()) {
        // Calculate delta time
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;

        // Update systems
        window.update();
        engine.update(deltaTime);

        // Render
        renderer.render();
    }

    // Shutdown
    renderer.shutdown();
    engine.shutdown();

    return 0;
}
```

**Validation**:
- [ ] Application compiles and links
- [ ] Window opens
- [ ] Clear color is displayed (cornflower blue)
- [ ] Application runs without crashing
- [ ] FPS is stable (should be capped by VSync at 60fps)
- [ ] Application closes cleanly (no leaks in debug mode)

---

## Build Integration

### Update CMakeLists.txt

Add new source files to `src/CMakeLists.txt`:
```cmake
target_sources(EntityMediaEngine PRIVATE
    core/Engine.cpp
    core/Window.cpp
    render/D3D12Renderer.cpp
    render/D3D12Device.cpp
)
```

Link D3D12 and DXGI:
```cmake
target_link_libraries(EntityMediaEngine PUBLIC
    d3d12.lib
    dxgi.lib
    dxguid.lib
)
```

---

## Validation Checklist

Phase 2 is complete when:

- [ ] **Window opens and displays clear color**
  - Window appears on screen
  - Background is cornflower blue (or chosen clear color)
  - Window is responsive to input

- [ ] **D3D12 device initializes successfully**
  - Device creation succeeds
  - No errors in debug output
  - Debug layer reports no issues

- [ ] **Swap chain presents frames**
  - Screen updates each frame
  - No tearing (VSync working)
  - Frame rate stable at 60fps

- [ ] **Debug layer shows no errors**
  - Run in debug mode
  - Check Visual Studio output window
  - No D3D12 warnings or errors

- [ ] **Application closes cleanly**
  - No crashes on exit
  - No memory leaks (check with debug CRT)
  - All COM objects released properly

---

## Common Issues

### Window doesn't appear

**Solutions**:
- Check GLFW initialization succeeded
- Verify glfwCreateWindow returned non-null
- Make sure glfwPollEvents() is being called

### D3D12 device creation fails

**Solutions**:
- Ensure Windows 10 SDK installed
- Check D3D12 feature level support (need 12.0+)
- Enable debug layer to see detailed errors
- Verify graphics driver is up to date

### Black screen instead of clear color

**Solutions**:
- Check swap chain format matches RTV format
- Verify command list is being executed
- Ensure Present() is being called
- Check resource state transitions are correct

### Validation errors in debug output

**Solutions**:
- Read error messages carefully
- Common issue: forgetting to close command list
- Common issue: incorrect resource state transitions
- Common issue: mismatched descriptor heap types

---

## Next Steps

After Phase 2 is complete:

1. **Proceed to Phase 3**: ECS Components & Systems
   - Define all core components
   - Create system base classes
   - Implement component registration

2. **Optional enhancements**:
   - Add FPS counter to window title
   - Implement proper fence-based GPU synchronization
   - Add basic error logging system

3. **Testing**:
   - Verify window resizing works
   - Test on different GPUs (NVIDIA, AMD, Intel)
   - Profile startup time and memory usage

---

## References

- [Microsoft D3D12 Hello Window Tutorial](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-basic-direct3d-12-component)
- [GLFW Documentation](https://www.glfw.org/documentation.html)
- [EnTT Wiki](https://github.com/skypjack/entt/wiki)

---

**Dependencies**: Phase 1 (Project Scaffold)
**Blocks**: Phase 3, 4, 5, 6, 7, 8 (all subsequent phases)
**Last Updated**: 2024-11-24
