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
    // `scale` is the post-conversion vertex multiplier
    // (Screen::size_meters / mesh native bounds), pre-computed on the editor
    // thread in PlaybackTimeAuthority::buildSceneSnapshot. The show thread
    // applies it directly via glm::scale; it never needs mesh bounds across
    // the bus. Wire-key intentionally retained for ABI stability per
    // include/entity/bus/CLAUDE.md rule 3.
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::uint64_t modelEntity{0}; // entt::entity cast to uint64; 0 = null
};

// One-frame snapshot of an OutputSurface entity (renamed from MappingSurface
// in Phase M1 of the two-tier mapping work, ADR-0021). The JSON wire keys
// inside this struct (entity, visible, corners, sourceUVs, softEdge*, …) and
// the array field name "surfaces" in SceneSnapshot / RenderFrame are pinned
// per bus rule 3 — only the C++ identifier moved.
struct OutputSurfaceSnapshot {
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

// One destination inside a content layer's routing (Plane A per ADR-0021).
// Mirrors entity::RouteTarget on the wire — screen ID + UV-space rectangle
// of the source content that lands on that screen. Wire-stable per bus
// rule 3 — new fields here must be optional with defaults. Declared early
// so both GenerativeLayerSnapshot and ClipCatalogEntry can carry vectors
// of these without forward-decl gymnastics.
struct ContentLayerRoute {
    std::uint64_t screen{UINT64_MAX};                                // UINT64_MAX = all screens
    std::array<float, 4> uvRect{0.0f, 0.0f, 1.0f, 1.0f};              // x, y, w, h in source UV
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

// One typed argument carried inside a SignalEmit (plugin event) or
// SignalLayerSnapshot (bake). Defined early so both can use it.
// Enums serialize as strings per bus rule 2.
struct SignalArg {
    enum class Type : int { Int = 0, Float = 1, String = 2 };

    Type    type{Type::Int};
    int32_t i{0};
    float   f{0.0f};
    std::string s;
};

// Editor → Show (D2R, in SceneSnapshot). One active Signal Output layer
// baked by PlaybackTimeAuthority::buildSceneSnapshot for SignalOutputSystem
// (Phase 5/6). Fields mirror SignalLayer + Layer window. Enums serialized
// as strings per bus rule 2. Break-tolerant decode: every field has a default.
//
// mode: 0=Momentary (fire once on crossing), 1=Continuous (every tick).
// transport: 0=OSC.
// valueSource: 0=Progress, 1=Keyframe (Continuous only).
// args: literal + value-bound args. valueBoundArgIndex == -1 if no bound arg.
// tracks: baked AnimatableProperty::SignalValue keyframes (Keyframe mode).
struct SignalLayerSnapshot {
    std::uint64_t entity{0};
    FrameNumber   startFrame{0};
    FrameNumber   duration{0};

    int           mode{0};          // SignalLayer::Mode as int
    int           transport{0};     // SignalLayer::Transport as int
    int           valueSource{0};   // SignalLayer::ValueSource as int

    std::string   address;

    // Args carried as SignalArg so SignalOutputSystem can hand them
    // directly to Engine::postSignalEmit without further conversion.
    std::vector<SignalArg>  args;
    int                     valueBoundArgIndex{-1};   // -1 = no bound arg

    // Range mapping for the value-bound arg.
    float srcMin{0.0f};
    float srcMax{1.0f};
    float outMin{0.0f};
    float outMax{1.0f};
    int   outType{0};   // SignalArg::Type as int

    // Baked keyframe tracks (SignalValue property) for Keyframe mode.
    // Show thread re-evaluates these per tick (NEW-07 pattern).
    std::vector<BakedTrack> tracks;
};

// One-frame snapshot of a Generative layer (Muncher v1; future kinds add
// optional fields here, never rename existing ones — bus rule 3). Baked by
// the editor thread in buildSceneSnapshot; consumed by CompositorSystem on
// the show thread to draw the procedural content. ADR-0014 path: the show
// thread never reads GenerativeLayer / MunchersGameState from the registry.
//
// kind: discriminator for future generative kinds (Particles, Visualizer,
//       …). v1 only ships Muncher. Encoded as an int on the wire so adding
//       a new kind doesn't break old payloads; serialization clamps unknown
//       values to Muncher.
// targetScreen: uint64 cast of the target Screen entity (UINT64_MAX = all).
// opacity / blendMode / zOrder: routing — pulled from the layer's MediaLayer
//       so the compositor can interleave generative draws with clips.
// muncher_simFrame: per-tick state from MunchersGameState. Show thread reads
//       this each render frame to drive the placeholder hue-cycle. Real
//       game state (Muncher pos, ghosts, pellets, score) lands here in v2.
struct GenerativeLayerSnapshot {
    enum class Kind : int { Muncher = 0, Text = 1 };

    std::uint64_t entity{0};
    Kind          kind{Kind::Muncher};
    std::uint64_t targetScreen{UINT64_MAX};

    // Plane A content routing (ADR-0021). Same contract as
    // ClipCatalogEntry: editor-baked from the GenerativeLayer entity's
    // ContentRouting component. Empty `routes` means fall back to
    // `targetScreen` above. Decoders default to empty/0 when absent.
    std::vector<ContentLayerRoute> routes;
    int                            mode{0};

    float         opacity{1.0f};
    int           blendMode{0};
    std::uint32_t zOrder{0};

    // UV-space affine transform of the layer in the target screen's NDC.
    // Identity by default. Used by PASS 2 compositor when blitting the
    // layer's compose-target RT onto the target screen (mirrors how
    // ClipCatalogEntry::transformMatrix is consumed for clips).
    std::array<float, 16> transformMatrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    // Compose-target slot allocated by CompositorSystem PASS 1 (R2D ack
    // writes GenerativeLayer::renderTargetSlot on the editor thread).
    // -1 = not yet allocated; PASS 2 skips the blit until the slot exists.
    std::int32_t  renderTargetSlot{-1};

    // Muncher-specific baked state. Show thread reads these to draw the
    // player + ghosts + pellets. The kind field above gates whether
    // they're meaningful — for Kind::Muncher all of them are live.
    std::uint64_t muncher_simFrame{0};
    float         muncher_x{0.5f};   // normalized [0, 1], origin top-left
    float         muncher_y{0.5f};
    float         muncher_inputX{0.0f};  // last-read external axes, debug overlays
    float         muncher_inputY{0.0f};

    // 3 ghosts × (x, y). Same coord convention as muncher_x/y.
    std::array<float, 6>          muncher_ghosts{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    // 16×16 wall bitset packed into 4 uint64. Bit set = wall cell.
    // Pellets are only present on cleared bits of this set.
    std::array<std::uint64_t, 4>  muncher_wallBits{0ull, 0ull, 0ull, 0ull};
    // 16×16 pellet bitset packed into 4 uint64. Bit set = pellet present.
    std::array<std::uint64_t, 4>  muncher_pelletBits{0ull, 0ull, 0ull, 0ull};
    std::uint16_t                 muncher_score{0};
    std::uint16_t                 muncher_lives{3};

    // Text-specific baked state. Meaningful only when kind == Text.
    // textTextureSlot is the video-pool descriptor slot TextSystem uploads
    // the rasterized RGBA8 frame into; -1 = not yet uploaded.
    std::int32_t  textTextureSlot{-1};
    std::uint32_t textBakedWidth{0};
    std::uint32_t textBakedHeight{0};

    // Layer window (timeline frames). Lets the show thread compute the
    // clip-local frame for animation re-evaluation.
    FrameNumber   startFrame{0};
    FrameNumber   duration{0};

    // Transform axes mirror (snapshot of Transform::{position,rotation,scale}).
    // Consulted by the show thread only when `tracks` is non-empty; for static
    // generative layers, `transformMatrix` above is authoritative.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // Euler degrees, X/Y/Z
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};

    // AnimatedProperties snapshot. Empty for generative layers without
    // animation. The show thread re-evaluates these per render frame at the
    // current Timeline frame so animation survives editor stalls (NEW-07).
    std::vector<BakedTrack> tracks;

    // ADR-0028: RemoteControlStore slot for this layer (-1 = not patched).
    // Baked from RemotePatch::storeSlot on the editor thread; consumed
    // lock-free by the show thread (indexed atomic loads only).
    std::int32_t remoteSlot{-1};
};

// One animated parameter track baked for show-thread re-evaluation. Mirror
// of an EffectAnimatedParameters::NamedTrack, but keyed by parameter
// *name* string (not the in-memory hash) per the wire-stability rule —
// the show side rehashes once on receipt. Keyframe payload is the same
// BakedKeyframe struct used for clip / OA animation.
struct BakedEffectTrack {
    std::string                paramName;
    bool                       enabled{true};
    std::vector<BakedKeyframe> keyframes;
};

// One effect node baked into the snapshot. Carries the kind hash, a
// flag, the marshalled parameter blob in cbuffer layout (filled in
// Phase 2 once an effect kind registry exists), and a parallel list of
// animated parameter tracks the show thread re-evaluates per render
// frame so animations stay alive during editor stalls (NEW-07 pattern).
//
// Editor-side bake order: static ParamValue → blob; then for each
// animated track, evaluate at the editor's current frame and overwrite
// the blob slot at the schema offset. The track itself is still copied
// across so the show side can re-evaluate during a stall.
struct EffectSnapshot {
    std::uint32_t                  kindIdHash{0};
    bool                           enabled{true};
    std::vector<std::uint8_t>      paramBlob;   // cbuffer-layout bytes
    std::vector<BakedEffectTrack>  tracks;
};

// Per-layer effects bundle. Side-table off SceneSnapshot — keyed by
// content-layer entity. The show side looks this up when building each
// ContentLayerSnapshot in buildRenderFrame and folds in `effects` plus
// the ping-pong render-target slots. Layers without an EffectChain
// component don't appear here; an absent entry on the show side means
// "no effects" and PASS 1.5 skips the layer.
struct LayerEffectsSnapshot {
    std::uint64_t                  entity{0};
    std::vector<EffectSnapshot>    effects;
    // Ping-pong compose-target slots allocated by PASS 1.5 via the R2D
    // ack pattern (`EffectChainRenderTargetAllocated`). -1 until both
    // sides are allocated; PASS 1.5 holds off running effects on this
    // layer until both are valid.
    std::int32_t                   slotA{-1};
    std::int32_t                   slotB{-1};
    std::uint32_t                  width{0};
    std::uint32_t                  height{0};
};

// Unified content-layer snapshot — the only data the compositor's PASS 2
// reads to composite a layer onto a screen. Kind-agnostic by design: any
// layer that produces a texture and lands on a screen (Clip-backed,
// Generative, future NDI/codec providers) shows up here. The editor-side
// bake (PlaybackTimeAuthority::buildSceneSnapshot) fills in one of these
// per active content layer, populating `sourceKind` + `sourceSlot` from
// the kind-specific component (VideoTexture::descriptorSlot for clips,
// GenerativeLayer::renderTargetSlot for generatives, …).
//
// ECS rule: composition selects the source — no Kind enum dispatch in
// the compositor. `sourceKind` here is wire-only — it tells the renderer
// which descriptor pool the slot indexes into (matches `TextureRef::Kind`).
struct ContentLayerSnapshot {
    enum class SourceKind : int { Video = 0, Compose = 1 };

    std::uint64_t entity{0};

    // Plane A content routing (ADR-0021). `routes` is the canonical
    // destination list — size 1 with identity uvRect for the M2 Direct
    // case (one screen, full-frame); size > 1 for the M3 Tiled case (one
    // source carved across N screens, each with its own UV crop). The
    // bake site expands the legacy "all visible screens" semantic into
    // one synthetic route per visible screen so PASS 2 sees an explicit
    // list with no special cases.
    //
    // `mode` matches entity::RouteMode (0=Direct, 1=Tiled). Cylindrical
    // / Spherical / Perspective are reserved (ADR-0021); decoders that
    // see unknown values must clamp to Direct rather than throwing.
    std::vector<ContentLayerRoute> routes;
    int                            mode{0};

    // Deprecated alias retained for one project-format / wire version
    // (ADR-0021 § Backward compatibility). Editor writes routes[0].screen
    // here when routes.size()==1, UINT64_MAX otherwise. Decoders prefer
    // `routes` if non-empty and fall back to materializing one route
    // from this field. Drop on Phase M4+1.
    std::uint64_t targetScreen{UINT64_MAX};   // UINT64_MAX = all screens
    std::array<float, 16> transformMatrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    float         opacity{1.0f};
    float         sectionFadeMultiplier{1.0f};
    int           blendMode{0};
    std::uint32_t zOrder{0};

    SourceKind    sourceKind{SourceKind::Video};
    std::int32_t  sourceSlot{-1};       // -1 = no texture yet (skip draw)

    // Optional colour-pipeline hints. PlaybackPresenter caches the
    // authoritative tags for clips; the compositor takes a best-effort
    // hint here and falls back to the presenter when bound to a video slot.
    int           colorSpace{0};         // TextureColorSpace as int
    std::string   ocioColorSpace;

    // Per-layer effect chain (ADR-0019 pending, issue #54). PASS 1.5 of
    // CompositorSystem walks this list when non-empty, ping-pongs two
    // compose targets, and writes the final result slot into
    // `postEffectsSlot`. PASS 2 then prefers postEffectsSlot over
    // sourceSlot when set. Effects evaluated in vector order for linear
    // stacks; topological order baked in by the editor for graph chains.
    // Empty vector = no effects, PASS 2 reads sourceSlot directly.
    std::vector<EffectSnapshot> effects;

    // Ping-pong compose-target slots for the effect chain (-1 = pending
    // allocation, R2D ack still in flight). Editor side fills these from
    // EffectChainRenderTargets on the layer entity during the buildRender-
    // Frame fold-in step, so PASS 1.5 reads them directly off the cl
    // entry without needing a side-table lookup.
    std::int32_t  effectChainSlotA{-1};
    std::int32_t  effectChainSlotB{-1};

    // Post-effects compose-target slot. -1 = no effects or PASS 1.5
    // hasn't produced output this frame yet (RT allocation pending).
    // Always indexes into the Compose descriptor pool when set.
    std::int32_t  postEffectsSlot{-1};

    // ADR-0028: RemoteControlStore slot carried from the source snapshot
    // (-1 = not patched). Not consumed directly by the compositor (the
    // overlay runs before this entry is built); retained for diagnostics
    // and future per-entry overlay extensions.
    std::int32_t  remoteSlot{-1};
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
    std::vector<OutputSurfaceSnapshot> surfaces;
    std::vector<ProjectorSnapshot>      projectors;
    std::vector<OutputSnapshot>         outputs;
    // Active generative layers (Muncher etc.) for this tick. Editor-baked,
    // already filtered to currently-active timeline frames. Drives PASS 1
    // (procedural draw into per-layer RT).
    std::vector<GenerativeLayerSnapshot> generativeLayers;
    // Unified content-layer list — PASS 2 compositor input. Sorted by
    // zOrder. Includes both clip-backed and generative entities; future
    // content kinds (NDI input, codec providers) add entries here without
    // touching the compositor's per-entry loop.
    std::vector<ContentLayerSnapshot>    contentLayers;
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
    // Total frames in the source media (Clip::totalMediaFrames). Needed by
    // effectivePlaybackLength's fallback when mediaOutFrame is still the
    // unresolved -1 sentinel (media missing / decoder not yet opened) —
    // without it the catalog reconstruction clamps such clips to a
    // 1-frame playback window (issue #74). 0 = unknown, matching the
    // component default.
    FrameNumber   totalMediaFrames{0};
    double        framerate{30.0};
    int           playbackMode{0};  // PlaybackMode enum as int
    int           sectionBehavior{0}; // SectionBehavior enum as int

    // 2026-05-23 — true when (startFrame + duration) exactly aligns with a
    // Section::breakFrame. Necessary but no longer sufficient for the
    // Normal-mode active-window extension: the bake-site also requires
    // `endingBreakFadeSeconds == 0` (see below). Computed editor-side
    // from sectionFadeTailFrames(endFrame) > 0; the show thread reads it
    // verbatim to keep mapToMediaFrameFromCatalog independent of Timeline
    // section state. ADR-0012 amendment 2026-05-23.
    bool          endAlignsWithSectionBreak{false};

    // 2026-05-23 follow-up — fadeSeconds of the section break at the
    // clip's endFrame, 0.0 if no aligned break or the break has no
    // fade. Shapes the Normal-mode extension (entity::computeExtendedDuration
    // in Clip.hpp): fadeSeconds == 0 extends to source-out (Fix 8's
    // original "play through the trim"); fadeSeconds > 0 extends through
    // the fade window only (clip.duration + ceil(fadeSeconds * fps)
    // timeline frames) so source content keeps advancing during the
    // visible fade-out then the decoder stops at fade end. The cap
    // fixes the two-clip-meeting-at-fade-break cache thrash (operator-
    // reported 2026-05-23: clip 1 + clip 2 both 4K H.264 at
    // fadeSeconds=5.0 exceeded the FrameCache budget and force-seek
    // thrashed past the fade window). ADR-0012 amendment 2026-05-23
    // follow-up.
    double        endingBreakFadeSeconds{0.0};

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

    // Plane A content routing (ADR-0021). `routes` is the canonical
    // destination list — populated by the editor bake from each clip's
    // ContentRouting component. `mode` matches entity::RouteMode (0=Direct,
    // 1=Tiled). Empty `routes` means the show-side bake should fall back to
    // the legacy single-target `targetScreen` field above. Wire-stable per
    // bus rule 3: decoders default to empty/0 when the keys are absent.
    std::vector<ContentLayerRoute> routes;
    int                            mode{0};

    // Resolved OCIO override (from ProjectManager, empty = use decoder default)
    std::string   ocioOverride;

    // ClipPlaybackPhase snapshot (only meaningful when hasPhase == true)
    bool          hasPhase{false};
    bool          phase_inContinuation{false};
    double        phase_sourcePhaseFrames{0.0};
    FrameNumber   phase_tailHoldMediaFrame{-1};
    FrameNumber   phase_postBreakMediaAnchor{-1};
    FrameNumber   phase_anchorTimelineFrame{0};

    // Wall-clock continuation anchor (NEW-08). Mirror of
    // ClipPlaybackPhase::{continuationStartTimeNs, continuationSeedFrames}.
    // When phase_continuationStartTimeNs > 0 the show thread re-derives the
    // live source phase from this anchor + steady_clock::now() instead of the
    // snapshot-frozen phase_sourcePhaseFrames — so Loop/PingPong clips keep
    // cycling at a parked section break while the editor thread is stalled.
    std::int64_t  phase_continuationStartTimeNs{0};
    double        phase_continuationSeedFrames{0.0};

    // Transform axes mirror (snapshot of Transform::{position,rotation,scale}).
    // Used by the show thread only when `tracks` is non-empty; for static
    // clips, `transformMatrix` above is the authoritative pre-bake.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // Euler degrees, X/Y/Z
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};

    // AnimatedProperties snapshot. Empty for clips without animation.
    std::vector<BakedTrack> tracks;

    // ADR-0028: RemoteControlStore slot (-1 = not patched). Baked from
    // RemotePatch::storeSlot editor-side; consumed lock-free show-side.
    std::int32_t remoteSlot{-1};
};

// One-frame snapshot of an ObjectAnimationLayer. Baked by the editor thread in
// buildSceneSnapshot; re-evaluated per render frame by the show thread in
// buildRenderFrame so OA animation stays alive during editor stalls (mirrors
// the NEW-07 fix for clips via applyBakedAnimation / BakedTrack).
//
// targetScreen: uint64 cast of the target Screen entity (0 = null / no target).
// startFrame / duration: OA layer window in timeline frames.
// trackIndex: used for priority resolution (lower wins) when multiple OA layers
//             target the same screen.
// tracks: keyframe tracks copied from AnimatedProperties (same BakedTrack shape
//         used by the clip catalog). Show thread evaluates these at
//         (currentFrame - startFrame) clamped to [0, duration).
// EndBehavior wire-format enum, mirrored from ObjectAnimationLayer::EndBehavior
// to avoid an entity-bus → core component dep. Serialized as a string ("Hold" /
// "Reset") per bus rule #2 (enums on the wire are strings).
enum class OAEndBehavior : std::uint8_t {
    Hold  = 0,
    Reset = 1,
};

struct ObjectAnimationLayerSnapshot {
    std::uint64_t            targetScreen{0};
    FrameNumber              startFrame{0};
    FrameNumber              duration{0};
    std::uint32_t            trackIndex{UINT32_MAX};
    OAEndBehavior            endBehavior{OAEndBehavior::Hold};
    std::vector<BakedTrack>  tracks;
};

// Stage 3: Editor → Show thread. Carries the scene-state portion of what
// buildRenderFrame used to snapshot inline. Published latest-wins on D2R
// from the editor half of the split buildRenderFrame once per editor frame.
// The show thread merges the most-recent cached SceneSnapshot into each
// per-tick RenderFrame alongside the show-derived activeClips / playState.
struct SceneSnapshot {
    std::vector<ScreenSnapshot>                screens;
    std::vector<OutputSurfaceSnapshot>        surfaces;
    std::vector<ProjectorSnapshot>             projectors;
    std::vector<OutputSnapshot>                outputs;
    // Clip catalog: all allocated + potentially-active clips snapshotted from
    // the registry on the editor thread. Show thread uses this instead of
    // touching Clip / VideoTexture / Transform / MediaLayer / ClipPlaybackPhase.
    std::vector<ClipCatalogEntry>              clipCatalog;
    // OA layer catalog: all active OA layers snapshotted from the registry.
    // Show thread re-evaluates tracks per render frame in buildRenderFrame and
    // folds results into the corresponding ScreenSnapshot position/rotation/scale.
    std::vector<ObjectAnimationLayerSnapshot>  objectAnimationLayers;
    // Generative layer catalog: all *active* generative layers (Muncher etc.).
    // PlaybackTimeAuthority::buildSceneSnapshot filters by current frame, so
    // the show thread iterates this list directly without re-checking start/
    // duration. Drives PASS 1 (per-layer procedural draw into a private RT).
    std::vector<GenerativeLayerSnapshot>       generativeLayers;
    // Unified content-layer catalog — PASS 2 compositor input. All *active*
    // entities that produce a texture and composite onto a screen (clip-
    // backed + generative + future kinds). Editor-baked, sorted by zOrder
    // ascending at publish time.
    std::vector<ContentLayerSnapshot>          contentLayers;
    // Per-layer effects side-table — looked up by entity when the show
    // thread builds each ContentLayerSnapshot in buildRenderFrame. Issue
    // #54. Absent entries = no effects.
    std::vector<LayerEffectsSnapshot>          layerEffects;
    // Signal Output Layer catalog — all active signal layers baked by
    // PlaybackTimeAuthority::buildSceneSnapshot. Show thread's
    // SignalOutputSystem (Phase 5/6) consumes this list directly without
    // re-checking the registry. Empty when no signal layers are on-timeline.
    std::vector<SignalLayerSnapshot>            signalLayers;
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

// Renderer → Director. The show thread created or destroyed an output
// window for an OutputDisplay entity (OutputManager::renderOutputs lazy
// creation / disable teardown / reconciliation). Director mirrors `slot`
// into OutputDisplay::outputWindowSlot on the editor thread — display-only
// state for the OutputsWindow UI; the authoritative slot map is
// show-thread-owned inside OutputManager (issue #76 closed the last
// show-thread registry write by moving it there). slot == UINT32_MAX means
// the window was destroyed.
struct OutputWindowSlotUpdated {
    std::uint64_t entity{0};
    std::uint32_t slot{UINT32_MAX};
};

// Renderer → Director. Mirror of ScreenRenderTargetAllocated for generative
// layers — the show thread's PASS 1 procedural render allocates a compose
// target per GenerativeLayer entity. Editor writes the slot back into
// GenerativeLayer::renderTargetSlot so the next SceneSnapshot's
// ContentLayerSnapshot for this layer carries the correct slot.
struct GenerativeLayerRenderTargetAllocated {
    std::uint64_t entity{0};
    std::uint32_t slot{UINT32_MAX};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// Renderer → Director. PASS 1.5 of CompositorSystem allocates two
// ping-pong compose targets per layer with effects (side 0 = slotA,
// side 1 = slotB). Editor drain writes the slot back into
// `EffectChainRenderTargets::slotA`/`slotB` on the layer entity so the
// next snapshot carries them. Two acks per layer-with-effects; one-time
// cost. Issue #54.
struct EffectChainRenderTargetAllocated {
    std::uint64_t entity{0};
    std::uint32_t slot{UINT32_MAX};
    std::uint8_t  side{0};      // 0 = A, 1 = B
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// Renderer → Director. A user-authored effect HLSL failed to compile
// (or an engine effect's .cso failed to load). Editor surfaces the
// error in the node graph UI (red badge on affected nodes) and falls
// back to a passthrough PSO so the rest of the chain keeps running.
// Issue #54, Phase 6.
struct EffectCompileFailed {
    std::uint32_t kindIdHash{0};
    std::string   errorMessage;
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

// Renderer → Director (NEW-08). The show thread's section-break detector
// found the playhead crossing a Timeline section break and has already
// snapped + paused the playhead and raised Timeline::sectionAtBreak(). The
// editor drains this in drainRendererToDirector and runs the registry-
// mutating catch-up via SectionScheduler::handleBreakAt (seed
// ClipPlaybackPhase, raise the scheduler's at-break latch). This keeps
// section breaks firing on time even while the editor thread is stalled.
struct SectionBreakDetected {
    FrameNumber breakFrame{0};  // integer timeline frame
};

// Show → Plugin (D2R, FIFO). Transport-neutral signal event posted by the
// show thread (SignalOutputSystem) and consumed by the OSC sender plugin.
// `address` is the OSC address pattern (e.g. "/entity/signal/foo").
// `args` is the ordered argument list. `transport` selects the outbound
// adapter (only OSC in Phase 1; MIDI, ArtNet, etc. are reserved values).
// Enums serialize as strings per bus rule 2.
struct SignalEmit {
    enum class Transport : int { OSC = 0 };

    Transport           transport{Transport::OSC};
    std::string         address;
    std::vector<SignalArg> args;
};

using Message = std::variant<
    RenderFrame,
    SceneSnapshot,
    RequestComposeCapture,
    CaptureCompleted,
    ProvisionClipResources,
    ResourcesProvisioned,
    ScreenRenderTargetAllocated,
    GenerativeLayerRenderTargetAllocated,
    EffectChainRenderTargetAllocated,
    EffectCompileFailed,
    OutputWindowSlotUpdated,
    SetOutputEnabled,
    ApplySettings,
    DeviceLost,
    FrameDropped,
    CreateOutputWindowRequest,
    OutputWindowReady,
    SectionBreakDetected,
    SignalEmit
>;

// Stable wire identifier. Mirrors the CommandDispatcher "type" string
// convention. Changing a name is a wire-format break.
const char* messageTypeName(const Message& msg) noexcept;

} // namespace entity::bus
