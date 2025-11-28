# Development History

Detailed completion notes for Entity Media Server phases.

---

## Phase 5: Projection Mapping (In Progress)

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
