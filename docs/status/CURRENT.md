# Current Status

**Phase**: Phase 5 - Projection Mapping
**Last Updated**: 2025-11-28

---

## In Progress: Multi-Render-Target Architecture

Implementing independent per-screen rendering for projection mapping (core feature).

### Problem
Target screen assignment wasn't working - clips assigned to specific screens showed on all screens or no screens. Root cause was single shared compose target for all screens.

### Solution
Multiple render targets with slot-based architecture - one compose target per screen.

### Component Status

| Component | Status | Details |
|-----------|--------|---------|
| D3D12Renderer | COMPLETE | Vector-based targets, slot-based API |
| CompositorSystem | PARTIAL | Needs full per-screen iteration loop |
| StageWindow | TODO | Needs per-screen texture support |

### D3D12Renderer Changes (Complete)
- Changed from single compose target to `vector<ComposeTarget>`
- Slot-based API: all methods accept `uint32_t slot` parameter
- `createComposeTarget()` returns slot ID instead of boolean
- ImGui descriptor heap layout: slot 0=fonts, 1=legacy, 2+=compose targets
- All getter methods support per-slot access

### CompositorSystem (Needs Work)
- Currently has simplified single-screen filtering
- NEEDS: Full per-screen iteration loop to composite each screen independently
- NEEDS: Initialize `renderTargetSlot` on Screen components

### StageWindow (TODO)
- NEEDS: Update to use per-screen textures from their respective slots
- Currently shows single legacy texture

---

## Next Steps

1. Implement full per-screen iteration in `CompositorSystem::update()`
2. Initialize `Screen::renderTargetSlot` when screens are created
3. Update `StageWindow::drawScreen()` to use per-screen compose target textures
4. Test multi-screen independent content rendering

---

## Active Files

**Primary**:
- [CompositorSystem.cpp](../../src/systems/CompositorSystem.cpp)
- [D3D12Renderer.cpp](../../src/render/D3D12Renderer.cpp)
- [StageWindow.cpp](../../src/ui/StageWindow.cpp)

**Supporting**:
- [Screen.hpp](../../include/entity/components/Screen.hpp)
- [D3D12Renderer.hpp](../../include/entity/render/D3D12Renderer.hpp)

---

## Architecture Notes

- Each Screen entity gets unique `renderTargetSlot` (0-based index)
- CompositorSystem should iterate all visible screens and composite each
- Clips filtered by `targetScreen` during composition (null = all screens)
- StageWindow 3D view displays each screen with its unique texture
