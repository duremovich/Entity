# Phase 3: ECS Components & Systems

**Status**: COMPLETE ✅
**Completed**: 2024-11-24
**Estimated Time**: 6-8 hours
**Dependencies**: Phase 2 complete

## Overview

Implement all core ECS components for Entity Media Server. These components represent the data structures that will be used throughout the application for managing media layers, clips, textures, and output configuration.

## Important: Pragmatic ECS Approach

**This phase implements a "Pragmatic ECS" approach** where components have some helper methods. While this doesn't follow pure ECS/Data-Oriented Design principles, it serves as a working foundation that will be refactored in Phase 4+.

### Why Pragmatic First?

1. **Validation**: Proves the ECS pattern works for our use case
2. **Learning**: Easier to understand component relationships
3. **Working Code**: Provides tests and safety net for refactoring
4. **Incremental**: Systems will be introduced naturally in Phase 4

### Refactoring Plan (Phase 4+)

The following components will be refactored to **pure data** structures:

| Component | Current (Pragmatic) | Target (Pure Data) | New System |
|-----------|-------------------|-------------------|------------|
| Transform | Has `updateMatrix()` | Remove methods | TransformSystem |
| MediaLayer | Has `shouldRender()` | Remove methods | RenderSystem |
| Clip | Has `containsFrame()` | Remove methods | TimelineSystem |
| TimelineTrack | Has `addClip()` | Remove methods | TrackSystem |
| VideoTexture | Has `isValid()` | Remove methods | TextureSystem |
| FrameBuffer | Has `hasFrames()` | Remove methods | BufferSystem |
| OutputMapping | Has `getAspectRatio()` | Remove methods | DisplaySystem |

**See**: `CLAUDE.md` → "ECS & Data-Oriented Design Principles" for full architectural guidelines.

## Goals

- Define all core ECS component structures ✅
- Implement component helper methods ✅
- Create test entities with components ✅
- Verify EnTT storage and retrieval works correctly ✅
- Set foundation for systems (Phase 4+) ✅

## Prerequisites

- Phase 2 complete (Engine, Window, D3D12Renderer working)
- EnTT library integrated
- Understanding of ECS architecture (components = data, systems = logic)

---

## Component List

All components go in `include/entity/components/` with implementations in corresponding `.cpp` files if needed.

### 1. Transform Component
**File**: `include/entity/components/Transform.hpp`
**Purpose**: Spatial transformation for layers

```cpp
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace entity {

struct Transform {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 rotation{0.0f, 0.0f, 0.0f};  // Euler angles in degrees
    glm::vec3 scale{1.0f, 1.0f, 1.0f};
    glm::mat4 matrix{1.0f};
    bool dirty{true};

    /**
     * Update the transformation matrix from position/rotation/scale.
     * Call this before rendering if dirty flag is true.
     */
    void updateMatrix();

    /**
     * Get the transformation matrix, updating if necessary.
     */
    const glm::mat4& getMatrix();
};

} // namespace entity
```

**Implementation** (`src/components/Transform.cpp`):
```cpp
#include "entity/components/Transform.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace entity {

void Transform::updateMatrix() {
    if (!dirty) return;

    // Build matrix: Translate * Rotate * Scale
    matrix = glm::mat4(1.0f);
    matrix = glm::translate(matrix, position);

    // Apply rotations (ZYX order)
    matrix = glm::rotate(matrix, glm::radians(rotation.z), glm::vec3(0, 0, 1));
    matrix = glm::rotate(matrix, glm::radians(rotation.y), glm::vec3(0, 1, 0));
    matrix = glm::rotate(matrix, glm::radians(rotation.x), glm::vec3(1, 0, 0));

    matrix = glm::scale(matrix, scale);

    dirty = false;
}

const glm::mat4& Transform::getMatrix() {
    updateMatrix();
    return matrix;
}

} // namespace entity
```

---

### 2. MediaLayer Component
**File**: `include/entity/components/MediaLayer.hpp`
**Purpose**: Layer rendering properties (z-order, opacity, blend mode, visibility)

```cpp
#pragma once

#include "entity/core/Types.hpp"
#include <cstdint>

namespace entity {

struct MediaLayer {
    uint32_t zOrder{0};           // Layer stacking order (higher = on top)
    float opacity{1.0f};          // 0.0 (transparent) to 1.0 (opaque)
    BlendMode blendMode{BlendMode::Normal};
    bool visible{true};

    /**
     * Check if this layer should be rendered.
     */
    bool shouldRender() const {
        return visible && opacity > 0.0f;
    }

    /**
     * Get effective opacity (clamped 0-1).
     */
    float getOpacity() const {
        return std::clamp(opacity, 0.0f, 1.0f);
    }
};

} // namespace entity
```

---

### 3. Clip Component
**File**: `include/entity/components/Clip.hpp`
**Purpose**: Media clip metadata (file path, codec info, timing)

```cpp
#pragma once

#include "entity/core/Types.hpp"
#include <string>
#include <cstdint>

namespace entity {

struct Clip {
    std::string filepath;              // Path to media file
    MediaType mediaType{MediaType::Unknown};

    // Timing (in frames)
    uint32_t startFrame{0};            // First frame of clip (in file)
    uint32_t duration{0};              // Total frames in clip
    uint32_t inPoint{0};               // Trim in point
    uint32_t outPoint{0};              // Trim out point

    // Codec info (set by decoder)
    uint32_t width{0};
    uint32_t height{0};
    float frameRate{30.0f};
    bool hasAlpha{false};

    /**
     * Get effective duration after trimming.
     */
    uint32_t getEffectiveDuration() const {
        if (outPoint > inPoint) {
            return outPoint - inPoint;
        }
        return duration;
    }

    /**
     * Check if clip is valid.
     */
    bool isValid() const {
        return !filepath.empty() && duration > 0;
    }
};

} // namespace entity
```

---

### 4. VideoTexture Component
**File**: `include/entity/components/VideoTexture.hpp`
**Purpose**: D3D12 GPU texture resources

```cpp
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace entity {

using Microsoft::WRL::ComPtr;

struct VideoTexture {
    ComPtr<ID3D12Resource> texture;           // GPU texture resource
    ComPtr<ID3D12Resource> uploadBuffer;      // Staging buffer for uploads

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{}; // Shader Resource View (CPU)
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle{}; // Shader Resource View (GPU)

    uint32_t width{0};
    uint32_t height{0};
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};

    bool isValid{false};

    /**
     * Check if texture is ready for rendering.
     */
    bool isReady() const {
        return isValid && texture != nullptr;
    }

    /**
     * Release all GPU resources.
     */
    void release() {
        texture.Reset();
        uploadBuffer.Reset();
        isValid = false;
    }
};

} // namespace entity
```

---

### 5. FrameBuffer Component
**File**: `include/entity/components/FrameBuffer.hpp`
**Purpose**: Circular ring buffer for decoded video frames

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include <atomic>

namespace entity {

// Single decoded frame
struct DecodedFrame {
    std::vector<uint8_t> data;    // Raw pixel data (RGBA)
    uint32_t frameNumber{0};
    uint32_t width{0};
    uint32_t height{0};
    bool valid{false};
};

struct FrameBuffer {
    static constexpr uint32_t MAX_FRAMES = 32;  // ~1 sec @ 30fps

    std::vector<DecodedFrame> frames;

    std::atomic<uint32_t> writeIndex{0};  // Next frame to write
    std::atomic<uint32_t> readIndex{0};   // Next frame to read
    std::atomic<uint32_t> frameCount{0};  // Number of buffered frames

    uint32_t capacity{MAX_FRAMES};

    /**
     * Initialize frame buffer with given capacity.
     */
    void initialize(uint32_t cap = MAX_FRAMES) {
        capacity = cap;
        frames.resize(capacity);
    }

    /**
     * Check if buffer has frames available.
     */
    bool hasFrames() const {
        return frameCount.load() > 0;
    }

    /**
     * Check if buffer is full.
     */
    bool isFull() const {
        return frameCount.load() >= capacity;
    }

    /**
     * Get next frame for reading (non-blocking).
     * Returns nullptr if no frames available.
     */
    DecodedFrame* getNextFrame() {
        if (!hasFrames()) return nullptr;

        uint32_t index = readIndex.load();
        DecodedFrame* frame = &frames[index];

        readIndex.store((index + 1) % capacity);
        frameCount.fetch_sub(1);

        return frame;
    }

    /**
     * Get next slot for writing (non-blocking).
     * Returns nullptr if buffer is full.
     */
    DecodedFrame* getWriteSlot() {
        if (isFull()) return nullptr;

        uint32_t index = writeIndex.load();
        DecodedFrame* frame = &frames[index];

        writeIndex.store((index + 1) % capacity);
        frameCount.fetch_add(1);

        return frame;
    }
};

} // namespace entity
```

---

### 6. TimelineTrack Component
**File**: `include/entity/components/TimelineTrack.hpp`
**Purpose**: Timeline track assignment

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <entt/entt.hpp>

namespace entity {

struct TimelineTrack {
    uint32_t trackIndex{0};              // Track number (0, 1, 2, ...)
    std::vector<entt::entity> clips;     // Entities on this track

    float yPosition{0.0f};               // Y position in timeline UI
    bool muted{false};
    bool locked{false};

    /**
     * Add clip entity to this track.
     */
    void addClip(entt::entity clipEntity) {
        clips.push_back(clipEntity);
    }

    /**
     * Remove clip entity from track.
     */
    void removeClip(entt::entity clipEntity) {
        auto it = std::find(clips.begin(), clips.end(), clipEntity);
        if (it != clips.end()) {
            clips.erase(it);
        }
    }
};

} // namespace entity
```

---

### 7. OutputMapping Component
**File**: `include/entity/components/OutputMapping.hpp`
**Purpose**: Display output configuration (EDID-based persistent mapping)

```cpp
#pragma once

#include <string>
#include <cstdint>

namespace entity {

struct OutputMapping {
    std::string edid;                // Display EDID (unique identifier)
    std::string displayName;          // User-friendly name

    uint32_t outputIndex{0};          // Physical output index

    int32_t positionX{0};             // Display position
    int32_t positionY{0};
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t refreshRate{60};

    bool primary{false};              // Is this the primary display?
    bool enabled{true};

    /**
     * Check if output is valid and enabled.
     */
    bool isActive() const {
        return enabled && width > 0 && height > 0;
    }
};

} // namespace entity
```

---

## Implementation Tasks

### Task 1: Implement Transform Component (1 hour)
**Files**:
- `include/entity/components/Transform.hpp`
- `src/components/Transform.cpp`

**Steps**:
1. Create header with Transform struct
2. Implement `updateMatrix()` using GLM
3. Add to CMakeLists.txt
4. Write simple test in main.cpp to verify matrix calculation

**Validation**:
- Matrix correctly represents TRS (translate-rotate-scale)
- Dirty flag works correctly
- getMatrix() updates when needed

---

### Task 2: Implement MediaLayer Component (30 min)
**Files**:
- `include/entity/components/MediaLayer.hpp`

**Steps**:
1. Create header with MediaLayer struct
2. Implement helper methods
3. Test with various opacity/visibility values

**Validation**:
- shouldRender() returns correct boolean
- getOpacity() clamps values correctly

---

### Task 3: Implement Clip Component (30 min)
**Files**:
- `include/entity/components/Clip.hpp`

**Steps**:
1. Create header with Clip struct
2. Implement helper methods
3. Test with sample file paths

**Validation**:
- getEffectiveDuration() handles trimming
- isValid() checks essential fields

---

### Task 4: Implement VideoTexture Component (30 min)
**Files**:
- `include/entity/components/VideoTexture.hpp`

**Steps**:
1. Create header with VideoTexture struct
2. Implement resource management methods
3. Verify ComPtr usage

**Validation**:
- isReady() checks texture validity
- release() cleans up resources

---

### Task 5: Implement FrameBuffer Component (1 hour)
**Files**:
- `include/entity/components/FrameBuffer.hpp`

**Steps**:
1. Create header with FrameBuffer struct
2. Implement ring buffer operations
3. Test thread-safety with atomics

**Validation**:
- Ring buffer wraps correctly
- Atomic operations prevent race conditions
- hasFrames()/isFull() return correct states

---

### Task 6: Implement TimelineTrack Component (30 min)
**Files**:
- `include/entity/components/TimelineTrack.hpp`

**Steps**:
1. Create header with TimelineTrack struct
2. Implement clip management methods
3. Test adding/removing clips

**Validation**:
- Clips can be added/removed
- Entity references stored correctly

---

### Task 7: Implement OutputMapping Component (30 min)
**Files**:
- `include/entity/components/OutputMapping.hpp`

**Steps**:
1. Create header with OutputMapping struct
2. Implement helper methods
3. Test with sample display data

**Validation**:
- isActive() checks essential fields
- Display configuration stored correctly

---

### Task 8: Component Registration & Testing (1-2 hours)
**File**: `src/core/Engine.cpp` or new `tests/test_components.cpp`

**Steps**:
1. Create test entities in Engine::initialize()
2. Register all component types
3. Verify EnTT storage/retrieval
4. Test component iteration with views

**Example Test Code**:
```cpp
// In Engine::initialize() or separate test
void Engine::testComponents() {
    // Create test entity
    auto entity = m_registry.create();

    // Add components
    m_registry.emplace<Transform>(entity);
    m_registry.emplace<MediaLayer>(entity, 0, 1.0f, BlendMode::Normal, true);
    m_registry.emplace<Clip>(entity, "test.mov", MediaType::VideoProRes4444);

    // Verify components exist
    assert(m_registry.all_of<Transform>(entity));
    assert(m_registry.all_of<MediaLayer>(entity));
    assert(m_registry.all_of<Clip>(entity));

    // Test component retrieval
    auto& transform = m_registry.get<Transform>(entity);
    transform.position = glm::vec3(100.0f, 200.0f, 0.0f);
    transform.updateMatrix();

    // Test view iteration
    auto view = m_registry.view<Transform, MediaLayer>();
    for (auto [entity, transform, layer] : view.each()) {
        std::cout << "Entity has Transform and MediaLayer" << std::endl;
    }

    std::cout << "Component tests passed!" << std::endl;
}
```

**Validation**:
- All components can be created
- Components can be added to entities
- Components can be retrieved
- Views iterate correctly
- No memory leaks

---

## Build Configuration

Update `CMakeLists.txt` to include component source files:

```cmake
target_sources(EntityMediaCore
    PRIVATE
        # Core
        src/core/Engine.cpp
        src/core/Application.cpp

        # Components
        src/components/Transform.cpp
        # (Other components are header-only)

        # Render
        src/render/D3D12Renderer.cpp
)
```

---

## Validation Checklist

Phase 3 is complete when:

- [ ] All 7 component headers created
- [ ] Transform.cpp implementation complete
- [ ] All components compile without errors
- [ ] Test entities can be created
- [ ] Components can be added to entities
- [ ] Components can be retrieved from entities
- [ ] EnTT views iterate correctly
- [ ] No memory leaks (verify with tools)
- [ ] Component tests pass
- [ ] Documentation updated

---

## Common Issues & Solutions

### Issue: GLM not found
**Solution**: Verify GLM is installed via vcpkg and included in CMakeLists.txt

### Issue: ComPtr not found
**Solution**: Include `<wrl/client.h>` for Windows Runtime Library smart pointers

### Issue: EnTT view syntax confusing
**Solution**: Use modern C++17 structured bindings: `for (auto [entity, comp1, comp2] : view.each())`

### Issue: Atomic operations unclear
**Solution**: FrameBuffer uses atomics for thread-safe ring buffer. Read EnTT threading docs.

---

## Next Steps

After Phase 3 completion:
- **Phase 4**: Media Decoding Pipeline (FFmpeg decoders, frame threading)
- **Phase 5**: D3D12 Rendering & Compositing (texture upload, shaders, multi-layer blend)
- **Phase 6**: Timeline Engine (transport controls, time mapping)

---

**Phase 3 Completion Target**: All ECS components implemented and tested, ready for systems integration.
