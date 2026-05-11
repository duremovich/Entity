// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "../core/AssetId.hpp"
#include "../core/Types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace entity::bus {

// Channel direction. D2R = Director→Renderer, R2D = Renderer→Director.
enum class Direction { D2R, R2D };

// ---------------------------------------------------------------------------
// Scene-snapshot types — carried on RenderFrame so the show thread reads
// scene state from the bus and never touches entt::registry directly.
// Stage 2: editor half of buildRenderFrame fills these from registry views.
// Stage 3: show thread only consumes them; no registry reads on show side.
// ---------------------------------------------------------------------------

// One-frame snapshot of a Screen entity.
// modelEntity is snapshotted as a uint64_t handle — the show thread never
// needs to touch the registry for geometry; OutputManager uses it as a key
// into a renderer-side Model store (Stage 4; for now it's informational).
struct ScreenSnapshot {
    std::uint64_t entity{0};
    std::string   name;
    bool          visible{true};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t renderTargetSlot{UINT32_MAX};
    bool          renderTargetValid{false};
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::uint64_t modelEntity{0}; // entt::entity cast to uint64; 0 = null
};

// One-frame snapshot of a MappingSurface entity.
struct MappingSurfaceSnapshot {
    std::uint64_t entity{0};
    bool          visible{true};
    std::uint32_t outputIndex{0};
    std::uint32_t surfaceIndex{0};
    std::array<std::array<float, 2>, 4> corners{{
        {-0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, -0.5f}, {-0.5f, -0.5f}}};
    std::array<std::array<float, 2>, 4> sourceUVs{{
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
    float softEdgeLeft{0.0f};
    float softEdgeRight{0.0f};
    float softEdgeTop{0.0f};
    float softEdgeBottom{0.0f};
    float brightness{1.0f};
    float gamma{1.0f};
};

// One calibration point for the residual-warp pass — must match CalibrationPoint layout.
struct CalibrationPointSnapshot {
    std::array<float, 3> worldPos{0.0f, 0.0f, 0.0f};
    std::array<float, 2> projectorUV{0.5f, 0.5f};
};

// One-frame snapshot of a Projector entity.
struct ProjectorSnapshot {
    std::uint64_t entity{0};
    std::array<float, 3> position{0.0f, 3.0f, 5.0f};
    std::array<float, 3> rotation{-20.0f, 0.0f, 0.0f};
    float fovDegrees{50.0f};
    float nearClip{0.1f};
    float farClip{50.0f};
    float distortionK1{0.0f};
    float distortionK2{0.0f};
    bool  useResidualWarp{false};
    bool  isCalibrated{false};
    // targetSurfaces: entity IDs of target Screen entities (up to 8).
    std::array<std::uint64_t, 8> targetSurfaces{};
    int   targetSurfaceCount{0};
    std::vector<CalibrationPointSnapshot> calibrationPoints;
};

// Calibration overlay snapshot — mirrors OutputDisplay::CalibrationOverlay
// but uses plain types (entity-bus has no glm dependency, per bus CLAUDE.md).
struct CalibrationOverlaySnapshot {
    bool                                       enabled{false};
    std::int32_t                               numPoints{0};
    std::array<std::array<float, 2>, 16>       points{};
    std::int32_t                               activeIndex{-1};
    bool                                       precisionCursor{false};
};

// One-frame snapshot of an OutputDisplay entity.
struct OutputSnapshot {
    std::uint64_t entity{0};
    std::string   name;
    int           outputType{0}; // 0=Physical,1=Preview,2=NDI,3=Virtual
    bool          enabled{true};
    bool          isPhysical{false};
    std::uint32_t outputWindowSlot{UINT32_MAX};
    std::int32_t  physicalDisplayIndex{0};
    std::int32_t  width{1920};
    std::int32_t  height{1080};
    // InputRegion (normalized coords)
    float inputRegionX{0.0f};
    float inputRegionY{0.0f};
    float inputRegionWidth{1.0f};
    float inputRegionHeight{1.0f};
    float brightness{1.0f};
    float gamma{1.0f};
    // Source routing: which projector / screen drives this output.
    std::uint64_t sourceProjector{0}; // 0 = null
    std::uint64_t sourceScreen{0};    // 0 = null
    CalibrationOverlaySnapshot calibrationOverlay{};
    // Window desktop position (for windowed physical outputs)
    std::int32_t  windowX{0};
    std::int32_t  windowY{0};
    std::uint32_t outputIndex{0}; // matches OutputDisplay::outputIndex for surface routing
};

// One active clip's contribution to a single rendered frame.
// `transformMatrix` is the pre-baked world matrix (column-major glm::mat4
// flattened). `ocioOverride` is the resolved per-clip OCIO input color space
// (empty string = use the decoder-stamped default).
struct ClipRenderState {
    std::uint64_t entity{0};
    int slot{-1};
    FrameNumber mediaFrame{0};
    std::string ocioOverride;
    std::array<float, 16> transformMatrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    float opacity{1.0f};
    BlendMode blendMode{BlendMode::Normal};
    std::uint64_t targetScreen{UINT64_MAX};
    // Section fade envelope (Phase D — auto-fade at section break boundaries).
    // Computed by PlaybackTimeAuthority each tick based on the clip's
    // start/end vs. surrounding Section break frames; 1.0 means no envelope.
    // CompositorSystem multiplies this into opacity at draw time.
    float sectionFadeMultiplier{1.0f};
    // Render z-order (lower values render first / behind). Sourced from
    // MediaLayer::zOrder; present on the bus so CompositorSystem can sort
    // without touching the registry.
    std::uint32_t zOrder{0};
};

// One frame the Renderer should warm into the FrameCache this tick.
struct WantedFrame {
    std::uint64_t entity{0};
    FrameNumber mediaFrame{0};
    int lookahead{0};
};

// Director → Renderer per-tick state snapshot. Becomes the only data path
// from Director to Renderer once subtask 8 lands.
struct RenderFrame {
    FrameNumber frameNumber{0};
    double deltaTime{0.0};
    TransportState playState{TransportState::Stopped};
    std::vector<ClipRenderState> activeClips;
    std::vector<WantedFrame> wantedFrames;
    // Stage 2: scene snapshot. Show thread reads these instead of registry.
    std::vector<ScreenSnapshot>         screens;
    std::vector<MappingSurfaceSnapshot> surfaces;
    std::vector<ProjectorSnapshot>      projectors;
    std::vector<OutputSnapshot>         outputs;
};

// One snapshot of an AnimatedProperties keyframe — bus-safe mirror of
// entity::Keyframe. The interpolation field encodes
// entity::InterpolationType as int (per the enum-as-string-on-wire rule
// in bus/CLAUDE.md, serialization translates).
struct BakedKeyframe {
    FrameNumber frame{0};
    float       value{0.0f};
    int         interpolation{0};  // InterpolationType enum as int
    float       easeIn{0.42f};
    float       easeOut{0.58f};
};

// One snapshot of an AnimatedProperties track — bus-safe mirror of
// entity::KeyframeTrack. `property` encodes entity::AnimatableProperty
// as int. Show thread re-evaluates these per render frame at
// Timeline::getCurrentFrame() so animation stays alive when the editor
// thread stalls and stops baking new snapshots (NEW-07).
struct BakedTrack {
    int                        property{0};   // AnimatableProperty enum as int
    bool                       enabled{true};
    std::vector<BakedKeyframe> keyframes;
};

// Per-clip catalog entry. Populated by PlaybackTimeAuthority::buildSceneSnapshot
// on the editor thread each frame. The show thread consumes this exclusively —
// it never reads Clip / VideoTexture / Transform / MediaLayer / ClipPlaybackPhase
// from the registry.
//
// transformMatrix: pre-baked world matrix (column-major glm::mat4). Editor
//   calls Transform::updateMatrix() here so the show side needs no const_cast.
// position / rotation / scale: the underlying Transform fields prior to
//   matrix bake. Carried so the show thread can re-evaluate animation
//   tracks and rebuild the matrix from these axes (overriding the
//   animated channels — PositionX/Y, Rotation Z, ScaleX/Y — while
//   keeping the unanimated axes intact). Only consulted when
//   `tracks` is non-empty; for static clips, transformMatrix is
//   authoritative.
// phase_*: snapshot of ClipPlaybackPhase (if the component exists). Shows
//   whether the clip is in section-continuation and carries its phase state.
// tracks: keyframe tracks copied from AnimatedProperties. Empty for
//   clips without animation. The show thread evaluates these on every
//   render frame at the current Timeline frame, so animation stays
//   alive during editor stalls (NEW-07).
struct ClipCatalogEntry {
    std::uint64_t entity{0};

    // Clip fields needed for isClipActiveAtFrame + mapToMediaFrame
    FrameNumber   startFrame{0};
    FrameNumber   duration{0};
    FrameNumber   mediaStartFrame{0};
    FrameNumber   mediaOutFrame{0};
    double        framerate{30.0};
    int           playbackMode{0};  // PlaybackMode enum as int
    int           sectionBehavior{0}; // SectionBehavior enum as int

    // VideoTexture
    int           descriptorSlot{-1};

    // Pre-baked transform matrix (column-major)
    std::array<float, 16> transformMatrix{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f
    };

    // MediaLayer
    float         opacity{1.0f};
    int           blendMode{0};     // BlendMode enum as int
    std::uint32_t zOrder{0};
    std::uint64_t targetScreen{UINT64_MAX};

    // Resolved OCIO override (from ProjectManager, empty = use decoder default)
    std::string   ocioOverride;

    // ClipPlaybackPhase snapshot (only meaningful when hasPhase == true)
    bool          hasPhase{false};
    bool          phase_inContinuation{false};
    double        phase_sourcePhaseFrames{0.0};
    FrameNumber   phase_tailHoldMediaFrame{-1};
    FrameNumber   phase_postBreakMediaAnchor{-1};
    FrameNumber   phase_anchorTimelineFrame{0};

    // Transform axes mirror (snapshot of Transform::{position,rotation,scale}).
    // Used by the show thread only when `tracks` is non-empty; for static
    // clips, `transformMatrix` above is the authoritative pre-bake.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // Euler degrees, X/Y/Z
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};

    // AnimatedProperties snapshot. Empty for clips without animation.
    std::vector<BakedTrack> tracks;
};

// Stage 3: Editor → Show thread. Carries the scene-state portion of what
// buildRenderFrame used to snapshot inline. Published latest-wins on D2R
// from the editor half of the split buildRenderFrame once per editor frame.
// The show thread merges the most-recent cached SceneSnapshot into each
// per-tick RenderFrame alongside the show-derived activeClips / playState.
struct SceneSnapshot {
    std::vector<ScreenSnapshot>         screens;
    std::vector<MappingSurfaceSnapshot> surfaces;
    std::vector<ProjectorSnapshot>      projectors;
    std::vector<OutputSnapshot>         outputs;
    // Clip catalog: all allocated + potentially-active clips snapshotted from
    // the registry on the editor thread. Show thread uses this instead of
    // touching Clip / VideoTexture / Transform / MediaLayer / ClipPlaybackPhase.
    std::vector<ClipCatalogEntry>       clipCatalog;
};

// Director → Renderer. Triggers the existing capture-pass pipeline; reply
// is `CaptureCompleted` keyed by `correlationId`.
//
// `hashOnly=true`  reads compose target `slot`, computes FNV-1a, returns
//                  the hex digest.
// `hashOnly=false` writes a PNG to `pngPath`. When `fullWindow=true` the
//                  source is the back buffer (entire window with UI);
//                  otherwise compose target `slot` (video output only).
struct RequestComposeCapture {
    std::uint64_t correlationId{0};
    int slot{0};
    bool hashOnly{true};
    bool fullWindow{false};
    std::string pngPath;
    std::string goldenHashPath;
};

// Renderer → Director. Resolves a parked script-result slot keyed by
// `correlationId`.
struct CaptureCompleted {
    std::uint64_t correlationId{0};
    std::string hexHash;
    bool ok{true};
    std::string errorMessage;
};

// Director → Renderer. Renderer allocates a VideoTexture descriptor slot
// and spawns the decode worker. Reply is `ResourcesProvisioned`.
//
// `assetId` is a strong-typed wrapper around the underlying string -- a
// path today; the seam where a future content-hash migration lands first.
struct ProvisionClipResources {
    std::uint64_t entity{0};
    AssetId assetId;
    MediaType mediaType{MediaType::Unknown};
    double framerate{30.0};
    FrameNumber totalMediaFrames{0};
};

// Renderer → Director. Director writes `descriptorSlot` back into the
// clip's VideoTexture component.
struct ResourcesProvisioned {
    std::uint64_t entity{0};
    int descriptorSlot{-1};
    bool ok{true};
    std::string errorMessage;
};

// Renderer → Director. CompositorSystem allocated a render-target slot for a
// Screen entity. Director writes `renderTargetSlot` / `renderTargetValid`
// back into the Screen component on the editor thread — same pattern as
// `ResourcesProvisioned`. Stage 4: replaces the last show-thread registry
// write so the narrow m_registryMutex lock can be removed.
struct ScreenRenderTargetAllocated {
    std::uint64_t entity{0};
    std::uint32_t slot{UINT32_MAX};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// Director → Renderer. Toggles a physical output's swap chain.
struct SetOutputEnabled {
    std::uint64_t entity{0};
    bool enabled{false};
};

// Director → Renderer. Mutable Settings fields the Renderer cares about.
struct ApplySettings {
    std::uint64_t frameCacheBytes{0};
    std::string ocioConfigPath;
};

// Renderer → Director. D3D12 device-removed event surfaced to the
// crash-recovery autosave path.
struct DeviceLost {
    int hresult{0};
    std::string reason;
};

// Renderer → Director. One frame failed to upload (decoder underrun, slot
// mismatch, …). Director uses this for diagnostics + drop-rate metrics.
struct FrameDropped {
    std::uint64_t entity{0};
    FrameNumber mediaFrame{0};
    std::string reason;
};

// Director → Renderer. Show thread should create a GLFW output window + D3D12
// swap chain for the given OutputDisplay entity. Reply is `OutputWindowReady`.
// Stage 4: bus type only. GLFW + swap chain wiring deferred until test gate.
struct CreateOutputWindowRequest {
    std::uint64_t entity{0};
    std::string   title;
    std::int32_t  x{0};
    std::int32_t  y{0};
    std::int32_t  width{1920};
    std::int32_t  height{1080};
    bool          borderless{false};
};

// Renderer → Director. Show thread finished creating the output window and
// swap chain. `ok=false` carries the reason in `errorMessage`.
struct OutputWindowReady {
    std::uint64_t entity{0};
    std::uint32_t outputWindowSlot{UINT32_MAX};
    bool          ok{true};
    std::string   errorMessage;
};

using Message = std::variant<
    RenderFrame,
    SceneSnapshot,
    RequestComposeCapture,
    CaptureCompleted,
    ProvisionClipResources,
    ResourcesProvisioned,
    ScreenRenderTargetAllocated,
    SetOutputEnabled,
    ApplySettings,
    DeviceLost,
    FrameDropped,
    CreateOutputWindowRequest,
    OutputWindowReady
>;

// Stable wire identifier. Mirrors the CommandDispatcher "type" string
// convention. Changing a name is a wire-format break.
const char* messageTypeName(const Message& msg) noexcept;

} // namespace entity::bus
