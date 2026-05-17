# Development History

Detailed completion notes for Entity Media Server phases.

---

## Phase D: Feature work (in progress)

### Content Routing Library + Feed Maps (2026-05-17)

ADR-0022 (six commits, L1-L5 + docs). Promotes Plane A content routing
from inline `ContentRouting` component (ADR-0021) to a **library of
named, reusable `ContentRoutingAsset` entities** that clips reference
via `ContentRoutingRef`. Adds a third routing kind, **Feed Map**, for
named-region authoring of LED-wall content.

**Data model.** New components in `include/entity/components/`:
`ContentRoutingAsset` (library entity with `kind`, `targets`,
`autoBoundScreen`, source size, autosync bookkeeping),
`ContentRoutingRef` (per-layer pointer to an asset). Helpers in
`ContentRoutingAssetOps.hpp` (`ensureAutoDirectAsset`,
`setLayerTargetScreen`, `setLayerCustomRouting`, `tryGetAsset`).
`RouteMode` extends to `{Direct, Tiled, FeedMap}`; `RouteTarget` gains
optional `name` for Feed Map region labels.

**System.** New `RoutingLibrarySystem` (editor-thread only) reconciles
the library every tick: creates one auto-direct asset per Screen,
name-syncs from `Screen::name` until the user diverges it
(`lastSyncedScreenName` bookkeeping), cascade-deletes orphan auto-direct
entries when a Screen is destroyed, clears any `ContentRoutingRef::asset`
that points at the deleted entry. Wired into `Director` alongside
`AnimationSystem`; ticked from `Engine::update` right after animation.

**Snapshot bake.** `PlaybackTimeAuthority::buildSceneSnapshot` resolves
`ContentRoutingRef → ContentRoutingAsset` on the editor thread before
populating `bus::ContentLayerSnapshot.routes`. The bus wire format
from ADR-0021 is unchanged — the show thread has zero awareness of the
asset library.

**UI.** `PropertyWindow`'s "Target Screen" relabels to "Content Routing"
and sources its dropdown from the library (auto-direct entries
alphabetical by Screen name, then user-created; "Default (All visible)"
sentinel at top). `ContentRoutingWindow` is a two-pane library
browser: "+ Add" with Direct / Tiled / Feed Map options, right-click
Delete with usage-count confirm dialog, per-kind detail editor on the
right with name input, kind dropdown, kind-specific authoring extras
(Tiled count/axis wizard; Feed Map source canvas size + Export
Template), targets table, and an interactive canvas.

**Feed Map.** Source canvas resolution + named per-target regions.
"Export Template..." writes an SVG to `<cwd>/.feed-templates/<name>.svg`
with outlined rects + region/screen labels — the deliverable for
content creators authoring matching source content. Targets table
displays pixel coordinates when kind = FeedMap (DragInt scaled by
`sourceWidth`/`Height`); UV storage and wire format unchanged.

**Canvas preview (L5).** When a clip is selected in the timeline, the
canvas background renders the clip's most-recently-uploaded video
frame. New `IRenderer::getVideoTextureIDForSlot(slot)` exposes the
per-slot SRV via `TextureUploader::gpuHandle`. Drag any region body
on the canvas to move it; drag any of four corner handles to resize.
Overlapping regions are allowed by design (content that repeats one
source slice to multiple screens). Hover tooltip shows pixel
coordinates for Feed Map, UV for Tiled.

**Persistence.** Project schema v19 → v20. New top-level
`contentRoutingAssets` array; per-layer `contentRoutingAssetName`
reference. v19 inline `contentRouting` JSON still read for one-version
backward compat. Migration on load: single-target Direct → existing
auto-direct asset; multi-target → fresh "Custom Routing N" asset;
empty targets → null ref ("all visible"). All four existing
`ContentRouting*` round-trip tests still pass.

**Files.** New: `ContentRoutingAsset.hpp`, `ContentRoutingRef.hpp`,
`ContentRoutingAssetOps.hpp`, `RoutingLibrarySystem.{hpp,cpp}`. Touched:
`PlaybackTimeAuthority.cpp` (bake), `PropertyWindow.cpp` (dropdown +
kind badge), `ContentRoutingWindow.{hpp,cpp}` (full rewrite to library
browser + canvas authoring), `Commands.cpp`
(`applyClipTargetScreen` + `applyContentRoutingSpec` route through
helpers), `ProjectSerializer.cpp` (v20 schema + v19 migration),
`Director.{hpp,cpp}` + `Engine.{hpp,cpp}` (system wiring),
`IRenderer.hpp` + `D3D12Renderer.{hpp,cpp}` (poster-frame slot
accessor), CMakeLists.txt, mock IRenderer in
`DirectorRendererRoundtripTests.cpp`. ADR `docs/adr/0022-...md` and
`ENTITY_ARCHETYPES.md` updated.

**Known deferred / out-of-scope** (not filed as roadmap cards):
`UndoableCommand` subclasses for library mutations (currently direct
registry writes — no undo for Create/Delete/Rename/SetField); drag-on-
empty-canvas to *create* regions ("+ Add Region" + table is the
authoring entry today); custom-font load to support real Unicode
glyphs in ImGui labels; region-overlap warnings (intentionally
allowed); off-canvas warnings.

**Commits:** `3ba7be0` (L1) → `229183f` (L2) → `af1c5f6` (L3) →
`f30ae12` (L4-minimal) → `b4de33a` (docs) → `9f6a44a` (L5).
510/510 ctest green.

---

### Editor/Show Thread Split (2026-05-08)

Issue #42 (five stages). Editor thread is now the **sole registry writer**.
Show thread owns GPU compositing and output Present; editor thread owns
registry, ImGui, project I/O, and command dispatch. Modal editor loops
cannot stall the projector pipeline.

**Stage 1** — Per-role D3D12 command allocators + fences
(`m_editorAllocators`, `m_showAllocators`, `m_editorFence`, `m_showFence`).
`beginShowFrame`/`endShowFrame` + `beginEditorFrame`/`endEditorFrame` replace
the deprecated `beginFrame`/`endFrame` single-list path throughout the
codebase. Both submit to the same direct command queue (D3D12-spec
thread-safe for `ExecuteCommandLists`/`Signal`).

**Stage 2** — `bus::SceneSnapshot` + `bus::RenderFrame`. Editor thread bakes
all per-clip state into `SceneSnapshot::clipCatalog` (`ClipCatalogEntry` per
clip: slot, opacity, blendMode, transformMatrix, targetScreen, zOrder,
sectionBehavior, …). Show thread calls `buildRenderFrame` from the snapshot
with zero registry reads for clip data.

**Stage 3** — Show thread spawned in `Engine::run`. `D2RChannel` carries
`RenderFrame` with latest-wins delivery (old snapshot superseded by new one
before the show thread drains). Show thread: drain D2R → `buildRenderFrame` →
`compositor->update` → `outputManager->render` → `endShowFrame`. Editor thread:
`buildSceneSnapshot` → send D2R → drain R2D → ImGui → `endEditorFrame`.

**Stage 4** — `Affinity` enum on `Command`; `processQueue(engine, affinity)`
skips wrong-affinity commands and re-queues them in order. Show thread drains
`Affinity::Show` (Play, Pause, Seek, SectionGo, …). Editor drains
`Affinity::Editor` + `Affinity::Either`. `ScreenRenderTargetAllocated` R2D
reply eliminates the last show-thread registry write: compositor posts it when
allocating a compose-target slot; editor writes `Screen::renderTargetSlot` in
`drainRendererToDirector`. `CreateOutputWindowRequest`/`OutputWindowReady`
bus types stub the GLFW/swap-chain handshake for output windows (wiring
deferred). `videoTex->descriptorSlot` data race fixed by gating on
`crs->slot >= 0` (baked from editor thread) instead of reading
`VideoTexture::descriptorSlot` on the show thread.

**Stage 5** — Remove deprecated `beginFrame`/`endFrame`; write ADR-0014;
update `CODE_ISSUES.md` (HIGH-02 fully closed, NEW-06 updated); update
`include/entity/bus/CLAUDE.md` with new message types.

**Files**: `src/core/Engine.cpp`, `src/render/D3D12Renderer.cpp`,
`include/entity/render/D3D12Renderer.hpp`, `include/entity/render/IRenderer.hpp`,
`src/systems/CompositorSystem.cpp`, `include/entity/systems/CompositorSystem.hpp`,
`src/command/CommandDispatcher.cpp`, `include/entity/command/Commands.hpp`,
`include/entity/bus/Message.hpp`, `src/bus/Serialization.cpp`,
`docs/adr/0014-editor-show-thread-split.md`.

---

### Tracy Profiler Integration (2026-05-10)

Issue #43 (eight phases). Live CPU/GPU profiling via Tracy 0.13.1 (`on-demand`
feature only; `gui-tools` excluded — POSIX `MAP_FAILED` breaks MSVC). See
ADR-0015 for full rationale and architecture.

**Build integration.** `ENTITY_ENABLE_TRACY` CMake option (default ON) gates
the dependency. `PUBLIC` compile definitions on `EntityMediaCore` propagate to
`EntityMediaEditor` and test targets. `entity-plugin-api` is intentionally
excluded. Tracy.exe is obtained from the v0.13.1 GitHub release, not vendored.

**Wrapper header.** `include/entity/profile/Tracy.hpp` — single include for all
Tracy macros. Provides full stubs (`using TracyD3D12Ctx = void*`, stub
`SetThreadName`, all macros) when disabled, so call sites need no per-line
`#ifdef` guards.

**Frame marks.** Two named frame contexts: `FrameMarkNamed("Editor")` in
`Engine::run` and `FrameMarkNamed("Show")` in `Engine::showThreadMain`, matching
the two independent render timelines from ADR-0014.

**D3D12 GPU zones.** Single `TracyD3D12Ctx` on `D3D12Renderer`, show-thread
only. A cross-function zone spans `beginShowFrame` → `endShowFrame` using a
heap-allocated `thread_local tracy::D3D12ZoneScope*` (non-movable type). The
delete site is at the very top of `endShowFrame`, before any early return
(device-lost or copy-list-close failure), so the destructor fires on every
code path.

**Per-frame plots.** Sampled once per show frame: `FrameCache bytes used`,
`FrameCache hit rate %`, `FrameCache entries`, `Decode queue depth` (Σ
`max(0, targetFrame − currentFrame)` across initialized decode workers).
`FrameCache::consumeAccessCounters()` atomically resets hit/miss counters
each interval.

**Thread names.** All worker threads named at startup: `"Decode #N"` (per
entity uint32), `"ContentScanner"`, `"MediaProbe"`, `"Transcode"`, `"OSC"`.
The OSC plugin uses `#if defined(TRACY_ENABLE) / #include <tracy/Tracy.hpp>`
directly (Apache-2.0 boundary — GPL wrapper excluded).

**TracyLockable.** `FrameCache::m_mutex` converted to
`TracyLockable(std::mutex, m_mutex)`; all 10 lock sites updated to
`std::lock_guard<LockableBase(std::mutex)>`.

**Files**: `include/entity/profile/Tracy.hpp`, `CMakeLists.txt`,
`vcpkg.json`, `src/core/Engine.cpp`, `src/render/D3D12Renderer.cpp`,
`include/entity/render/D3D12Renderer.hpp`, `src/systems/DecodeSystem.cpp`,
`src/media/FrameCache.cpp`, `include/entity/media/FrameCache.hpp`,
`src/project/ContentScanner.cpp`, `src/media/MediaProbeWorker.cpp`,
`src/media/TranscodeWorker.cpp`, `plugins/osc-receiver/OscReceiverPlugin.cpp`,
`docs/adr/0015-profiling-with-tracy.md`.

---

### OSC Receiver Plugin + Preferences (2026-05-08)

Inbound OSC over UDP for triggering Entity from external show-control
software (Network Cues, stage-manager consoles, Companion, TouchOSC).
Hand-rolled OSC 1.0 parser; Winsock listener on port 53000 (default,
configurable via Preferences); routes a fixed namespace into Director
commands:

```
/entity/play  /entity/pause  /entity/stop
/entity/section/next
/entity/cue/{number}/go
/entity/seek <int frame>
```

**First control-plane plugin shipped.** Routing departs from ADR-0005's
stated bus-based model — control-plane plugins call
`CommandDispatcher::enqueue(typeName, paramsJson)` via narrow
`IPluginContext` accessors instead. Bus stays Director↔Renderer-only
until Phase E forces multi-process. Documented in ADR-0013.

**Plugin-API additions** (additive at vtable bottom, no
`PLUGIN_API_VERSION` bump):
- `enqueueCommand(typeName, paramsJson)`
- `registerShutdownHook(fn)` — engine joins worker threads before
  tearing down dispatcher/bus
- `getBoolSetting` / `getIntSetting` — narrow stringly-typed Settings
  accessors so the GPL Settings struct stays out of the Apache-2.0
  header

**Settings UI**: new "OSC Receiver" section in Preferences (enable
checkbox + listener port, restart-required). Defaults: enabled, 53000.

**File-association support**: editor accepts a positional
`<project.entity>` argument; Windows hands this to the editor on
double-click after the user sets a file association. Falls back to
the launcher on load failure so a stale association doesn't dead-end.

**Files**: `plugins/osc-receiver/`,
`plugin-api/include/entity/plugin/PluginContext.hpp`,
`include/entity/core/{Settings,Engine,EnginePluginContext}.hpp`,
`src/core/{Settings,Engine,EnginePluginContext}.cpp`,
`src/ui/SettingsWindow.cpp`, `apps/editor/main.cpp`,
`docs/adr/0013-control-plane-plugins-route-via-command-dispatcher.md`.

**Roadmap status**: Issue #2 ("Phase D: OSC control surface") still
open — outbound OSC sender and per-project custom mapping UI remain
as future subtasks.

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
