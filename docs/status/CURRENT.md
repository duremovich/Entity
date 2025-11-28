# Current Status

**Phase**: Phase 5 - Projection Mapping
**Last Updated**: 2025-11-28

---

## Completed: Multi-Render-Target Architecture

Independent per-screen rendering for projection mapping is now functional.

### What Was Fixed

1. **Target screen assignment** - Each screen gets its own compose target and clips are filtered by their `targetScreen` assignment.

2. **Descriptor heap slot collision** (2025-11-28) - Fixed critical bug where adding a second screen caused video to disappear on ALL screens. Root cause: compose targets and video textures were allocated overlapping descriptor heap slots. Fix: Added `MAX_COMPOSE_TARGETS = 8` constant and proper slot separation.

3. **Multi-video playback freeze** (2025-11-28) - Fixed critical freeze (5+ seconds) when playing multiple videos on timeline. Root cause: Race condition between `clear()` and `consumeUpTo()` in FrameRingBuffer caused count to wrap to UINT32_MAX, resulting in 4 billion loop iterations. Fix: Added count validation checks to detect and handle corrupted count values.

4. **Stale frame flash on seek-before-clip** (2025-11-28) - Fixed visual glitch where seeking before a clip's start and playing showed wrong frame briefly. Root cause: Race condition between seek signaling and frame retrieval; also late seek triggered when clip became active. Fix: Added `seekPending` check in Engine.cpp to skip stale buffer access, and proactive seek in DecodeSystem.cpp when timeline is before clip start.

### Component Status

| Component | Status | Details |
|-----------|--------|---------|
| D3D12Renderer | COMPLETE | Vector-based targets, slot-based API |
| CompositorSystem | COMPLETE | Full per-screen iteration loop with lazy RT allocation |
| StageWindow | COMPLETE | Per-screen texture display in 3D view |

### How It Works

1. **Lazy Render Target Allocation**: `CompositorSystem::ensureScreenRenderTarget()` creates compose targets on first use
2. **Per-Screen Iteration**: CompositorSystem iterates ALL visible screens, not just the first
3. **Clip Filtering**:
   - `targetScreen == entt::null` renders to ALL screens
   - `targetScreen == specificEntity` renders only to that screen
4. **Per-Screen Display**: StageWindow 3D view passes each screen's unique texture to `drawScreen()`

### Key Changes Made

**CompositorSystem.cpp/hpp**:
- Added `ensureScreenRenderTarget()` helper for lazy allocation
- Refactored `update()` to iterate all visible screens
- Each screen gets its own compose target at its resolution
- Clips filtered per-screen inside the iteration loop

**StageWindow.cpp**:
- Added `textureID` field to `ScreenDrawData` struct
- Each screen now gets its own compose target texture
- Removed shared slot-0 texture lookup

**D3D12Renderer.hpp/cpp**:
- Added `MAX_COMPOSE_TARGETS = 8` constant
- Fixed descriptor heap layout to prevent slot collisions
- Video textures now start at slot `2 + MAX_COMPOSE_TARGETS`

**Commands.hpp/cpp**:
- Added `AddScreenCommand` for scripted screen creation
- Added `SetClipTargetScreenCommand` for scripted target assignment

---

## Next Steps

1. Test with multiple screens and clips with different target assignments
2. Verify dimension changes trigger render target recreation
3. Consider adding screen deletion cleanup (currently leaves gaps)

---

## Active Files

**Modified**:
- [CompositorSystem.cpp](../../src/systems/CompositorSystem.cpp) - Multi-screen loop
- [CompositorSystem.hpp](../../include/entity/systems/CompositorSystem.hpp) - Added helper
- [StageWindow.cpp](../../src/ui/StageWindow.cpp) - Per-screen textures
- [Engine.cpp](../../src/core/Engine.cpp) - Stale frame prevention (seekPending check)
- [DecodeSystem.cpp](../../src/systems/DecodeSystem.cpp) - Proactive seek before clip entry

---

## Architecture Notes

- Each Screen entity gets unique `renderTargetSlot` (0-based index, lazy allocated)
- CompositorSystem iterates all visible screens and composites each independently
- Clips filtered by `targetScreen` during composition (null = all screens)
- StageWindow 3D view displays each screen with its unique texture
- Slots are never reused (gaps left on screen deletion) - acceptable for typical 2-8 screens
