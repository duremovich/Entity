#pragma once

// =============================================================================
// ENTITY COMPONENT SYSTEM & DATA-ORIENTED DESIGN GUIDELINES
// =============================================================================
//
// This file documents the architectural principles for Entity Media Server.
// All code must follow these patterns for performance and maintainability.
//
// Quick Reference:
// - Components = Pure Data (POD structs, no methods except trivial helpers)
// - Systems = Pure Logic (classes that operate on component views)
// - Performance: Cache-friendly iteration, lock-free threading
// - EnTT: Sparse sets architecture for O(1) component access
//
// Last Updated: 2024-11-24
// =============================================================================

namespace entity {

// =============================================================================
// 1. THE GOLDEN RULES
// =============================================================================

// -----------------------------------------------------------------------------
// Rule 1: Components = Pure Data
// -----------------------------------------------------------------------------
//
// Components MUST be POD (Plain Old Data) structs with public data members.
// They represent the "state" of entities, not their "behavior".
//
// ✅ CORRECT: Pure data component
//
//   struct Transform {
//       glm::vec3 position{0.0f};
//       glm::vec3 rotation{0.0f};
//       glm::vec3 scale{1.0f};
//       glm::mat4 matrix{1.0f};  // Cached result
//       bool dirty{true};        // State flag
//   };
//
// ❌ INCORRECT: Logic in component
//
//   struct Transform {
//       glm::vec3 position{0.0f};
//       void updateMatrix() { /* ... */ }   // NO! Logic belongs in System
//       glm::mat4 getMatrix() { /* ... */ } // NO! This is behavior, not data
//   };
//
// Component Design Rules:
// - Components are **structs** with public data members
// - Prefer POD (Plain Old Data) / aggregate types when possible
// - No virtual functions, no inheritance hierarchies
// - No business logic or complex calculations
// - Trivial constructors/destructors only
// - Keep components small (< 64 bytes ideal for cache coherency)
//
// Acceptable Component Methods:
// - Default constructors with member initializers
// - Simple setters that ONLY set a flag (e.g., dirty = true)
// - Deleted copy/move constructors if needed for unique resources
// - RAII destructors for resource cleanup (e.g., ComPtr::Reset())
//
// Why This Matters:
// - EnTT packs components contiguously in memory (sparse sets)
// - POD components = cache-friendly iteration = 10-100x performance gain
// - No vtables = less memory overhead, better cache utilization
// - Trivial copy/move enables lock-free operations

// -----------------------------------------------------------------------------
// Rule 2: Systems = Pure Logic
// -----------------------------------------------------------------------------
//
// Systems are classes that encapsulate behavior and operate on component data.
// They represent the "behavior" that acts on entity "state".
//
// ✅ CORRECT: System operates on component data
//
//   class TransformSystem {
//   public:
//       void update(entt::registry& registry) {
//           auto view = registry.view<Transform>();
//           for (auto [entity, transform] : view.each()) {
//               if (transform.dirty) {
//                   // Calculate matrix from position/rotation/scale
//                   transform.matrix = calculateMatrix(transform);
//                   transform.dirty = false;
//               }
//           }
//       }
//   private:
//       glm::mat4 calculateMatrix(const Transform& t) { /* ... */ }
//   };
//
// System Design Rules:
// - Systems are **classes** that encapsulate behavior
// - Systems operate on **views** of components via EnTT registry
// - Systems are stateless (or have minimal state like caches)
// - Systems never store entity IDs long-term
// - Systems communicate via components, not direct references
//
// When to Move Logic to System:
// - Any calculation (matrix math, frame mapping, etc.)
// - Any iteration or search
// - Any state changes affecting multiple entities
// - Any I/O or external calls

// -----------------------------------------------------------------------------
// Rule 3: Why This Matters (Performance)
// -----------------------------------------------------------------------------
//
// Cache Coherency:
//
//   // EnTT stores components in packed arrays (sparse set)
//   // When iterating a view<Transform>, components are contiguous in memory:
//   // [Transform1][Transform2][Transform3][Transform4]... ← Cache-friendly!
//   //
//   // With logic in components, memory layout is unpredictable:
//   // [Transform1+vtable][padding][Transform2+vtable]... ← Cache misses!
//
// Parallelization:
// - Pure data components allow lock-free parallel system execution
// - Systems operating on disjoint component sets can run concurrently
// - No hidden state means no race conditions
//
// Debugging:
// - Components are easily inspectable (just data)
// - System logic is centralized and testable
// - No spaghetti method calls between components
//
// Memory Efficiency:
// - POD components → Trivial copy/move
// - No vtables → Less memory overhead
// - Dense packing → Better cache utilization
//
// Expected Performance (1000 entities):
// - POD component iteration: < 10 microseconds
// - Non-POD with methods: 20-50 microseconds
// - Component with virtuals: 100+ microseconds

// =============================================================================
// 2. ENTT-SPECIFIC BEST PRACTICES
// =============================================================================

// -----------------------------------------------------------------------------
// Sparse Sets Architecture
// -----------------------------------------------------------------------------
//
// EnTT uses **sparse sets** for component storage:
//
//   Sparse Array (entity → component index):  [2, _, 0, 1, _, 3]
//   Dense Array (components):                 [C0, C1, C2, C3]
//
// Benefits:
// - O(1) component lookup
// - O(1) component add/remove
// - Dense iteration (cache-friendly)
// - Components packed contiguous in memory
//
// To Maximize Performance:
// - Keep components small (< 64 bytes ideal)
// - Use POD types when possible
// - Avoid pointers/references in components (breaks locality)
// - Use shared_ptr only when necessary (prefer component composition)

// -----------------------------------------------------------------------------
// View Iteration Patterns
// -----------------------------------------------------------------------------
//
// Single component:
//
//   auto view = registry.view<Transform>();
//   for (auto [entity, transform] : view.each()) {
//       // Process transform
//   }
//
// Multiple components (intersection):
//
//   auto view = registry.view<Transform, MediaLayer>();
//   for (auto [entity, transform, layer] : view.each()) {
//       // Process entities with both Transform AND MediaLayer
//   }
//
// Excluding components:
//
//   auto view = registry.view<Transform>(entt::exclude<Disabled>);
//   for (auto [entity, transform] : view.each()) {
//       // Process transforms, but skip entities with Disabled component
//   }
//
// Parallel iteration (EnTT 3.11+):
//
//   auto view = registry.view<Transform>();
//   view.each([](auto entity, Transform& transform) {
//       // Process in parallel (thread-safe if no shared state)
//   });

// =============================================================================
// 3. PRAGMATIC EXCEPTIONS
// =============================================================================

// -----------------------------------------------------------------------------
// When Helper Methods Are Acceptable
// -----------------------------------------------------------------------------
//
// 1. Trivial Getters (inline, no computation):
//
//   struct MediaLayer {
//       float opacity{1.0f};
//       bool visible{true};
//
//       // OK: Trivial boolean check
//       bool isVisible() const { return visible && opacity > 0.0f; }
//   };
//
// 2. Setters with Dirty Flags:
//
//   struct Transform {
//       glm::vec3 position{0.0f};
//       bool dirty{true};
//
//       // OK: Just sets data + flag
//       void setPosition(const glm::vec3& pos) {
//           position = pos;
//           dirty = true;
//       }
//   };
//
// 3. Resource Management (RAII):
//
//   struct VideoTexture {
//       ComPtr<ID3D12Resource> texture;
//
//       // OK: RAII cleanup
//       ~VideoTexture() { texture.Reset(); }
//   };
//
// Anti-Patterns to Avoid:
//
// ❌ DON'T: Complex calculations in components
//
//   struct Transform {
//       void updateMatrix() { /* matrix math */ }  // Move to TransformSystem!
//   };
//
// ❌ DON'T: Virtual functions
//
//   struct Component {
//       virtual void update() = 0;  // This breaks cache coherency!
//   };
//
// ❌ DON'T: Component-to-component calls
//
//   struct ClipA {
//       void doSomething(ClipB& other) { }  // Components shouldn't interact!
//   };

// =============================================================================
// 4. SYSTEM DESIGN PATTERNS
// =============================================================================

// -----------------------------------------------------------------------------
// System Lifecycle
// -----------------------------------------------------------------------------
//
// class ExampleSystem {
// public:
//     // Called once during engine initialization
//     void initialize(entt::registry& registry) {
//         // Setup initial state, allocate resources
//     }
//
//     // Called every frame
//     void update(entt::registry& registry, double deltaTime) {
//         auto view = registry.view<ComponentA, ComponentB>();
//         for (auto [entity, a, b] : view.each()) {
//             // Process components
//         }
//     }
//
//     // Called once during engine shutdown
//     void shutdown(entt::registry& registry) {
//         // Cleanup resources
//     }
// };

// -----------------------------------------------------------------------------
// System Communication
// -----------------------------------------------------------------------------
//
// Systems communicate via:
// 1. **Components** (primary): Write results to components
// 2. **Events** (EnTT signals): For decoupled messaging
// 3. **Shared resources**: Via Engine class (renderer, etc.)
//
// Example: Via components (preferred)
//
//   class TimelineSystem {
//       void update(entt::registry& registry, double time) {
//           auto view = registry.view<Clip, FrameBuffer>();
//           for (auto [entity, clip, buffer] : view.each()) {
//               buffer.targetFrame = calculateFrame(clip, time);
//           }
//       }
//   };
//
//   class DecodeSystem {
//       void update(entt::registry& registry) {
//           auto view = registry.view<Clip, FrameBuffer>();
//           for (auto [entity, clip, buffer] : view.each()) {
//               // Read targetFrame, decode, write to buffer
//           }
//       }
//   };

// -----------------------------------------------------------------------------
// System Update Order
// -----------------------------------------------------------------------------
//
// Systems must execute in dependency order:
//
//   void Engine::update() {
//       // 1. Input & Timeline
//       m_timelineSystem->update(m_registry, m_deltaTime);
//
//       // 2. Transform Updates
//       m_transformSystem->update(m_registry);
//
//       // 3. Media Decoding (reads timeline state)
//       m_decodeSystem->update(m_registry);
//
//       // 4. Compositor (reads transforms & textures)
//       m_compositorSystem->update(m_registry);
//
//       // 5. Render (reads compositor output)
//       m_renderSystem->update(m_registry);
//   }

// =============================================================================
// 5. REFACTORING PLAN (Phase 4+)
// =============================================================================
//
// Current Implementation Status (As of 2024-11-24):
// Our Phase 3 components follow a "Pragmatic ECS" approach with some helper
// methods. This was acceptable to establish the foundation, but Phase 4+ will
// refactor to Pure Data components.
//
// Component Refactoring Roadmap:
//
// | Component      | Current State          | Target State | New System       |
// |----------------|------------------------|--------------|------------------|
// | Transform      | Has updateMatrix()     | Pure data    | TransformSystem  |
// | MediaLayer     | Has shouldRender()     | Pure data    | RenderSystem     |
// | Clip           | Has containsFrame()    | Pure data    | TimelineSystem   |
// | TimelineTrack  | Has addClip()          | Pure data    | TrackSystem      |
// | VideoTexture   | Has isValid()          | Pure data    | TextureSystem    |
// | FrameBuffer    | Has hasFrames()        | Pure data    | BufferSystem     |
// | OutputMapping  | Has getAspectRatio()   | Pure data    | DisplaySystem    |
//
// Why Not Refactor Immediately?
// - Phase 3 validates the ECS pattern works
// - Systems will be created naturally in Phase 4 (Media Decoding)
// - Easier to understand migration path with working code
// - Tests provide safety net for refactoring

// =============================================================================
// 6. PERFORMANCE TESTING
// =============================================================================

// -----------------------------------------------------------------------------
// Benchmarking Component Iteration
// -----------------------------------------------------------------------------
//
// Measure iteration speed:
//
//   auto start = std::chrono::high_resolution_clock::now();
//
//   auto view = registry.view<Transform, MediaLayer>();
//   for (auto [entity, transform, layer] : view.each()) {
//       // Process components
//   }
//
//   auto end = std::chrono::high_resolution_clock::now();
//   auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
//
// Expected Results (1000 entities):
// - POD component iteration: < 10 microseconds
// - Non-POD with methods: 20-50 microseconds
// - Component with virtuals: 100+ microseconds

// =============================================================================
// 7. THREADING GUIDELINES
// =============================================================================

// -----------------------------------------------------------------------------
// Threading Model
// -----------------------------------------------------------------------------
//
// Entity Media Server uses three primary thread types:
//
// 1. Main Thread:
//    - ECS updates (systems)
//    - UI rendering (ImGui)
//    - Timeline control
//    - Compositor
//
// 2. Decode Threads (per-clip):
//    - FFmpeg decode workers
//    - Write to FrameBuffer components
//    - MUST NOT block main thread
//
// 3. D3D12 Command Queue:
//    - Asynchronous GPU work
//    - Texture uploads
//    - Rendering
//
// Thread Safety Rules:
// - Use std::atomic for thread-safe flags (e.g., buffer ready flags)
// - Avoid heavy mutexes in hot paths (decode/render loops)
// - Lock-free operations preferred
// - Components accessed from multiple threads need synchronization
//
// Example: Lock-Free Frame Buffer
//
//   struct FrameBuffer {
//       std::array<AVFrame*, 32> frames{nullptr};
//       std::atomic<uint32_t> writeIndex{0};
//       std::atomic<uint32_t> readIndex{0};
//       // Decode thread writes to writeIndex
//       // Main thread reads from readIndex
//   };

// =============================================================================
// 8. REFERENCES & FURTHER READING
// =============================================================================
//
// EnTT Documentation:
// - https://github.com/skypjack/entt/wiki
// - https://github.com/skypjack/entt/wiki/Crash-Course:-entity-component-system
//
// Data-Oriented Design:
// - https://www.dataorienteddesign.com/dodbook/
// - Mike Acton: Data-Oriented Design (CppCon 2014)
//   https://www.youtube.com/watch?v=rX0ItVEVjHc
// - Stoyan Nikolov: "OOP Is Dead, Long Live Data-oriented Design" (CppCon 2018)
//   https://www.youtube.com/watch?v=yy8jQgmhbAU
//
// Performance:
// - "Game Engine Architecture" by Jason Gregory (Chapter 15: Runtime Gameplay Foundation Systems)
// - "Data-Oriented Design" by Richard Fabian
// - EnTT performance benchmarks: https://github.com/skypjack/entt/wiki/Benchmarks

} // namespace entity
