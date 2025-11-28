# Current Status

**Phase**: Phase 5 - Projection Mapping
**Last Updated**: 2025-11-28

---

## Completed: Mixed Frame Rate Support

Videos now play at their native frame rate regardless of timeline frame rate. A 24fps video on a 30fps timeline plays at correct speed with proper duration display.

### What Was Fixed

1. **Frame rate conversion** (2025-11-28) - Fixed critical bug where videos played at timeline frame rate instead of source frame rate. A 24fps video on 30fps timeline was playing 25% faster and ending early.

   **Root cause**: 1:1 frame mapping between timeline and source frames, ignoring source video's native frame rate.

   **Fix**: Added frame rate ratio conversion throughout the codebase:
   - `Engine::mapToMediaFrame()` - Converts timeline frames to source frames
   - `DecodeSystem::update()` - Uses frame rate ratio for target frame calculation
   - `clip.duration` - Now stored in timeline frames (source frames × timelineFPS/sourceFPS)
   - `TimelineWidget` - All frame↔time conversions use timeline frame rate

2. **Added `frameBlending` field to Clip component** - Per-clip option for smoother playback at mismatched frame rates (not yet implemented in renderer).

### Previous Fixes (2025-11-28)

3. **Descriptor heap slot collision** - Fixed video disappearing when adding second screen.

4. **Multi-video playback freeze** - Fixed 5+ second freeze from FrameRingBuffer race condition.

5. **Stale frame flash on seek-before-clip** - Fixed wrong frame showing briefly after seek.

6. **Playback freeze after backward scrubbing** - Fixed freeze when dragging playhead backwards.

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

**Modified for Frame Rate Support**:
- [Clip.hpp](../../include/entity/components/Clip.hpp) - Added `frameBlending` field
- [Engine.cpp](../../src/core/Engine.cpp) - Frame rate conversion in `mapToMediaFrame()`, duration calculation
- [DecodeSystem.cpp](../../src/systems/DecodeSystem.cpp) - Frame rate ratio for target frame calculation
- [TimelineWidget.cpp](../../src/timeline/TimelineWidget.cpp) - All frame↔time conversions use timeline rate
- [Timeline.cpp](../../src/timeline/Timeline.cpp) - Fixed split/duplicate for mixed frame rates
- [ProjectSerializer.cpp](../../src/project/ProjectSerializer.cpp) - Recalculates duration on project load
- [MediaBinWindow.cpp](../../src/ui/MediaBinWindow.cpp) - Shows source duration correctly

---

## Architecture Notes

### Frame Rate Handling
- **Timeline frame rate**: Fixed rate for the project (e.g., 30fps) - set once, not changed by clips
- **Clip frame rate**: Source video's native rate (e.g., 24fps) - stored in `clip.framerate`
- **Duration**: Stored in timeline frames (`clip.duration = totalMediaFrames × timelineFPS/sourceFPS`)
- **Frame mapping**: `Engine::mapToMediaFrame()` converts timeline frames to source frames
- **Formula**: `sourceFrame = timelineLocalFrame × (sourceFPS / timelineFPS)`

### Multi-Screen Rendering
- Each Screen entity gets unique `renderTargetSlot` (0-based index, lazy allocated)
- CompositorSystem iterates all visible screens and composites each independently
- Clips filtered by `targetScreen` during composition (null = all screens)
- StageWindow 3D view displays each screen with its unique texture
- Slots are never reused (gaps left on screen deletion) - acceptable for typical 2-8 screens
