# Development History

Detailed completion notes for Entity Media Server phases.

---

## Phase 5: Projection Mapping (In Progress)

### Mixed Frame Rate Support (2025-11-28)

Fixed critical bug where videos played at timeline frame rate instead of their native frame rate. A 24fps video on a 30fps timeline was playing 25% faster and ending early.

**Root Cause**: 1:1 frame mapping between timeline frames and source frames. The system treated frame numbers as interchangeable regardless of frame rate.

**Fix**: Added frame rate ratio conversion throughout the codebase:

1. **Duration calculation**: `clip.duration` now stored in timeline frames:
   ```
   duration = totalMediaFrames × (timelineFPS / sourceFPS)
   ```

2. **Frame mapping**: `Engine::mapToMediaFrame()` converts timeline frames to source frames:
   ```
   sourceFrame = timelineLocalFrame × (sourceFPS / timelineFPS)
   ```

3. **Timeline display**: All frame↔time conversions in `TimelineWidget.cpp` now use timeline frame rate instead of clip frame rate.

4. **Clip operations**: `Timeline::splitClip()` and `duplicateClip()` properly handle mixed frame rates.

5. **Project loading**: `ProjectSerializer` recalculates duration from `totalMediaFrames` to handle old project files.

**New Features**:
- Added `frameBlending` field to Clip component (renderer support pending)
- Timeline frame rate is no longer changed when loading clips

**Files**: Clip.hpp, Engine.cpp, DecodeSystem.cpp, TimelineWidget.cpp, Timeline.cpp, ProjectSerializer.cpp, MediaBinWindow.cpp

---

### Multi-Screen Targeting Fix (2025-11-28)

Fixed critical descriptor heap slot collision that caused video to disappear when a second screen was created.

**Root Cause**: Compose targets and video textures were allocated overlapping descriptor heap slots:
- Compose targets used slots `2 + slot` (0, 1, 2, ...)
- Video textures used slots `3 + slot` (0, 1, 2, ...)
- When compose target 1 was created (heap slot 3), it overwrote video texture 0's SRV

**Fix**:
- Added `MAX_COMPOSE_TARGETS = 8` constant to D3D12Renderer
- Fixed heap layout: compose targets at slots 2-9, video textures at slots 10+
- Video textures now use slot `2 + MAX_COMPOSE_TARGETS + slot`

**New Script Commands**:
- `AddScreen` - Create new screen via script
- `SetClipTargetScreen` - Set clip's target screen via script

**Files**: D3D12Renderer.hpp/cpp, Commands.hpp/cpp, CommandDispatcher.cpp

---

### Scrubbing/Seek Freeze Fix (2025-11-28)

Fixed playback freeze when dragging playhead or clips backwards then pressing play.

**Root Cause**: Multiple interacting issues:
1. Race condition: decode thread used stale `targetFrame` after seek was signaled
2. Buffer thrashing: rapid seeks during drag operations cleared buffer repeatedly
3. Duplicate seeks: single timeline click triggered multiple seek calls

**Fix**:
1. Update `targetFrame` atomically before signaling seek in `seekClip()`
2. Added seek debouncing in TimelineWidget (only seek when time actually changes)
3. Added scrubbing mode that prevents decoder seeks during drag operations:
   - Timeline sets `m_isScrubbing = true` when drag starts
   - DecodeSystem skips seek detection while scrubbing
   - On drag release, scrubbing ends and one final seek is triggered
   - Applied to ruler drag, clip drag, and clip trim operations

**Files**: DecodeSystem.cpp/hpp, Timeline.hpp, TimelineWidget.cpp/hpp

---

### Window Management Improvements (2025-11-27)

Implemented per-window undocking via right-click context menu and fixed ping-pong playback mode.

**Window Management Features**:
- Layout Lock: Windows locked by default to prevent accidental undocking
- Right-click Undock: Right-click on any window tab to undock/dock individual windows
- Visual Feedback: Undocked windows shrink by 10 pixels for clear indication
- Menu Access: Windows > Dock/Undock submenu for all windows
- Keyboard Shortcut: Ctrl+L to toggle layout lock
- State Sync: Dock state syncs with ImGui reality

**Ping-Pong Playback Fixes**:
- Increased buffer capacity for ping-pong clips (up to 256 frames)
- Fixed buffer stalling at 0/32 by using `getFrame()` instead of `consumeUpTo()`
- Improved seek logic for bidirectional playback
- Added reverse phase detection for proper backward playback

**Files**: WindowManager.hpp/cpp, DecodeSystem.cpp, Engine.cpp

---

### 3D Stage Visualization (2025-11-26)

Implemented disguise-style 3D stage visualization with floor grid and camera controls.

**Features**:
- Floor Grid: Major/minor grid lines with perspective, colored coordinate axes (red X, blue Z)
- Screen Quad: Composited video displayed on 3D quad in space (16:9 aspect, elevated above floor)
- Camera Controls: Orbit (left drag), Pan (middle/shift+left), Zoom (scroll/right drag)
- View Presets: Reset, Front, Top, Side buttons in toolbar
- 2D/3D Toggle: Switch between flat composited view and 3D stage visualization

**Files Created**: Camera.hpp, Stage3DRenderer.hpp/cpp
**Files Modified**: StageWindow.hpp/cpp

---

### Keyframe UI Enhancements (2025-11-25)

Improved keyframe editing interface in both timeline and properties panel.

**Timeline Enhancements**:
- Expanded clip view shows property tracks (Position X/Y, Scale X/Y, Rotation, Opacity)
- Left-hand track header with property names, stopwatch icons, keyframe navigator
- Keyframe diamonds on property tracks show keyframe positions
- Click on clip arrow to expand/collapse property tracks

**Properties Panel**:
- Each animatable property has keyframe controls
- Stopwatch button (gold when has keyframes, gray when static)
- `<` / `>` Navigate to previous/next keyframe
- Diamond button to add/remove keyframe at current frame
- Clicking navigation buttons seeks timeline to keyframe position

**Files**: TimelineWidget.cpp, PropertyWindow.cpp

---

### Keyframe Animation System (2025-11-24)

Implemented property keyframing for clip animation with linear interpolation.

**Animatable Properties**: PositionX, PositionY (NDC), Rotation (degrees), ScaleX, ScaleY, Opacity

**Interpolation Types**: Linear (default), Step (hold), EaseInOut (cubic bezier placeholder)

**Components**: AnimatedProperties.hpp, AnimationSystem.hpp/cpp

---

## Phase 4: Media Decoding Pipeline (Complete)

### PNGSequenceDecoder (2025-11-24)

Implemented PNG sequence decoder with stb_image library.

**Features**:
- Directory scanning for all PNG files
- Alphabetical sorting for deterministic sequence ordering
- Alpha channel detection from first frame
- Premultiplied alpha conversion
- Full error handling

**Files**: PNGSequenceDecoder.hpp/cpp

### ProResDecoder (2025-11-23)

FFmpeg-based ProRes 4444 decoding with YUV to RGBA conversion.

### Interactive Timeline UI (2025-11-23)

**Features**:
- Timeline Zoom: Alt + Mouse Wheel (10-500 px/sec)
- Ruler Scrubbing: Click and drag on time ruler
- Clip Repositioning: Click and drag clips
- Deferred resize pattern for D3D12 stability

---

## Phase 3: ECS Components (Complete)

- System infrastructure (System.hpp base class)
- Component refactoring (Clip, FrameBuffer as pure data)
- FrameRingBuffer (lock-free circular buffer)
- Decoder base class + implementations

---

## Phase 2: Core Engine (Complete)

- D3D12 renderer initialization
- ImGui integration
- Window management
- Basic rendering pipeline

---

## Phase 1: Project Scaffold (Complete)

- CMake/vcpkg build system
- Directory structure
- Dependency setup (EnTT, FFmpeg, ImGui, GLFW)
