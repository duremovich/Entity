#pragma once

/**
 * IRenderer - Backend-agnostic rendering interface
 *
 * Pure-virtual interface that systems, UI, and the Engine depend on so neither
 * D3D12 nor any future backend (Metal for Mac) leaks out of src/render/. See
 * the Phase B architecture decision in the plan file for rationale.
 *
 * The D3D12 implementation lives at src/render/D3D12Renderer.{hpp,cpp}.
 * A future Metal implementation would live alongside as a peer class.
 */

#include "entity/core/Types.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

// Forward declarations
struct GLFWwindow;

namespace entity {

// Forward declaration — UploadHeapBuffer is defined in UploadHeapBufferPool.hpp.
// IRenderer.hpp forward-declares it so the copyUploadBufferToVideoTextureSlot
// default no-op compiles without pulling in D3D12 headers (UploadHeapBufferPool.hpp
// includes <d3d12.h>). Concrete backends that override the method include the
// full header in their own header (D3D12Renderer.hpp already does so).
struct UploadHeapBuffer;

/**
 * Opaque texture reference — which pool the handle indexes is encoded in Kind.
 * Callers pass these around; the renderer resolves to backend-specific
 * descriptors internally. Keeps D3D12 types out of shared headers.
 */
struct TextureRef {
    enum class Kind : uint8_t {
        Invalid,
        VideoSlot,      // Allocated via IRenderer::allocateVideoTextureSlot
        ComposeTarget,  // Allocated via IRenderer::createComposeTarget
    };
    Kind kind{Kind::Invalid};
    uint32_t slot{0};
    // #90 per-slot reuse generation the renderer checks against
    // IRenderer::videoTextureSlotGeneration when resolving a VideoSlot draw.
    // UINT32_MAX = "any generation" (skip the check) — the default for callers
    // that don't thread a generation (legacy/compose/generative paths).
    uint32_t generation{UINT32_MAX};

    bool valid() const { return kind != Kind::Invalid; }

    static TextureRef invalid()                        { return {}; }
    static TextureRef video(uint32_t s)                { return {Kind::VideoSlot, s}; }
    static TextureRef video(uint32_t s, uint32_t gen)  { return {Kind::VideoSlot, s, gen}; }
    static TextureRef compose(uint32_t s)              { return {Kind::ComposeTarget, s}; }
};

class IRenderer {
public:
    // Limits callers need to know about. Must match backend implementation.
    // (Video-texture pool size lives in DescriptorHeapLayout::MAX_VIDEO_TEXTURE_SLOTS;
    // allocateVideoTextureSlot() returns UINT32_MAX on exhaustion.)
    // Compose-target pool — shared between screens, generative-layer RTs
    // (ADR-0018), and per-layer effect ping-pongs (issue #54, 2 RTs per
    // layer with effects). Bumped from 32 → 64 when the effects system
    // landed so a show with ~6 screens + ~10 generatives + ~10 effects-
    // enabled layers fits comfortably. Raise (or split into per-kind
    // pools) when an installation pushes total active slots past this.
    static constexpr uint32_t MAX_COMPOSE_TARGETS           = 64;
    // 8192 leaves room for projector-output mesh rendering with per-triangle
    // barycentric subdivision in OutputManager (each mesh triangle becomes N×N
    // sub-triangles, each a drawOutputSurface call). 256-byte aligned slots ×
    // FRAME_COUNT ≈ 6 MB, negligible.
    static constexpr uint32_t MAX_MAPPING_SURFACES_PER_FRAME = 8192;

    virtual ~IRenderer() = default;

    // ------------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------------
    virtual Result initialize(GLFWwindow* window, uint32_t width, uint32_t height) = 0;
    virtual void   shutdown() = 0;
    virtual Result resize(uint32_t width, uint32_t height) = 0;
    virtual bool     isInitialized() const = 0;
    virtual bool     isDeviceLost() const = 0;
    // Returns the HRESULT from GetDeviceRemovedReason() captured at the time
    // of detection; 0 (S_OK) until device loss occurs. Typed as int32_t to
    // keep Windows types out of this backend-agnostic header.
    virtual int32_t  getDeviceLostReason() const = 0;

    // ------------------------------------------------------------------------
    // Per-frame
    // ------------------------------------------------------------------------
    virtual void     clear(float r, float g, float b, float a) = 0;
    virtual uint32_t getCurrentBackBufferIndex() const = 0;
    virtual void     beginImGuiFrame() = 0;
    virtual void     endImGuiFrame() = 0;

    // Stage 1 A/B-list split.
    // Show role: copy-queue uploads, compositor draws, output-window Present.
    // Editor role: back-buffer transition, ImGui recording, editor Present.
    virtual void beginShowFrame() = 0;
    virtual void endShowFrame() = 0;
    virtual void beginEditorFrame() = 0;
    virtual void endEditorFrame() = 0;

    // ------------------------------------------------------------------------
    // Video texture slots (per-clip video uploads)
    // ------------------------------------------------------------------------
    virtual uint32_t   allocateVideoTextureSlot() = 0;
    // Defer a slot free to the show thread, which reclaims it once nothing —
    // in-flight GPU work on any queue, or an editor ImGui frame holding the
    // slot's SRV — can still reference it (#89; see the beginShowFrame drain).
    // This is the ONLY way to free a slot: a synchronous editor-thread free
    // can never be made safe against the show thread's mid-record frame or
    // the calling frame's own recorded ImGui commands.
    // Default no-op for mock/test renderers.
    virtual void       scheduleVideoTextureSlotFree(uint32_t /*slot*/) {}
    // Returns the number of currently allocated video texture slots (including
    // pre-allocated but not yet freed). Useful for integration tests that
    // verify slot leak detection (AssertVideoTextureSlotsAllocated command).
    virtual uint32_t   getVideoTextureSlotsAllocated() const { return 0; }
    // #90 — current reuse generation of a video texture slot. Captured right
    // after allocateVideoTextureSlot() (provision / project-load) and threaded
    // through the SceneSnapshot so a stale cached snapshot that reuses the same
    // slot index has its uploads/draws rejected. Default 0 for mock/test
    // renderers (they never reuse a slot against a stale snapshot).
    virtual uint32_t   videoTextureSlotGeneration(uint32_t /*slot*/) const { return 0; }
    // #90 — count of uploads rejected on a generation mismatch. Polled by the
    // AssertVideoTextureStaleGenerationSkips integration command. Default 0.
    virtual uint64_t   videoTextureStaleGenerationSkips() const { return 0; }
    // 2026-05-23 — Proactively create the slot's GPU texture + upload buffer
    // so the first uploadVideoFrameToSlot doesn't pay two CreateCommittedResource
    // calls (~33MB each for 4K) on the show thread. Closes the section-break
    // stutter where a queued-at-break clip's first-frame upload allocated
    // the texture lazily on the show thread, blocking the render frame for
    // ~30-100ms. Idempotent on matching dimensions. Returns true on success.
    // Default implementation is a no-op for mock renderers; D3D12Renderer
    // delegates to TextureUploader::prepareTexture.
    virtual bool       prepareVideoTextureSlot(uint32_t /*slot*/,
                                               uint32_t /*width*/,
                                               uint32_t /*height*/,
                                               TextureFormat /*format*/ =
                                                   TextureFormat::RGBA8_UNORM) {
        return true;
    }
    // Upload an RGBA8 frame to a video slot. Legacy convenience — equivalent
    // to uploadVideoFrameToSlot(slot, data, w, h, TextureFormat::RGBA8_UNORM).
    virtual bool       uploadVideoFrameToSlot(uint32_t slot,
                                              const uint8_t* rgba,
                                              uint32_t width,
                                              uint32_t height) = 0;
    // Upload a frame in any supported format (RGBA8 or pre-compressed BC*).
    // `data` is the raw decoded bytes; size is inferred from `width`, `height`
    // and `format` (RGBA = w*h*4, BC* = block-packed). For HAP playback this
    // uploads pre-compressed blocks directly to a BC-format GPU texture
    // with zero CPU decompression.
    // `expectedGeneration` (#90): the slot reuse generation captured when this
    // clip's slot was resolved. UINT32_MAX (default) skips the check. A
    // mismatch means the slot index was freed + reassigned since the cached
    // snapshot was baked — the upload is rejected (returns false) so a stale
    // snapshot can't overwrite another clip's slot.
    virtual bool       uploadVideoFrameToSlot(uint32_t slot,
                                              const uint8_t* data,
                                              uint32_t width,
                                              uint32_t height,
                                              TextureFormat format,
                                              uint32_t expectedGeneration = UINT32_MAX) = 0;
    // Editor-thread upload. uploadVideoFrameToSlot records into the
    // show-thread-owned copy command list, which the show thread Resets
    // every beginShowFrame — a copy recorded from the editor thread is
    // discarded before it executes. This variant records into a dedicated
    // editor-owned copy list and executes it immediately. Use it for any
    // video-slot upload issued from the editor thread (e.g. TextSystem
    // rasterized text). RGBA8 only. See ADR-0014.
    virtual bool       uploadVideoFrameToSlotImmediate(uint32_t slot,
                                                       const uint8_t* rgba,
                                                       uint32_t width,
                                                       uint32_t height) = 0;

    // Phase 5 — GPU-direct copy path for UploadHeap-backed FrameCache frames.
    //
    // Records a CopyTextureRegion from `buf->resource` (an UploadHeapBuffer
    // written by a decode worker) into the video slot's DEFAULT-heap texture,
    // bypassing the show-thread CPU memcpy that `uploadVideoFrameToSlot` performs.
    // After recording, calls `UploadHeapBufferPool::recordCopyDone` on the buffer
    // so its fence-guarded deleter waits for GPU completion before recycling.
    //
    // The concrete backend (D3D12Renderer) reads its current show-fence value
    // internally — callers do not need to pass it. Default no-op for mock/test
    // renderers (they never receive UploadHeap frames because the allocator
    // isn't wired). D3D12Renderer overrides this.
    //
    // Returns true if the copy was recorded into the show command list (caller
    // may advance lastDecodedFrame). Returns false if the call was skipped or
    // failed (no copy recorded; caller should NOT advance lastDecodedFrame —
    // retry next tick). Mocks return false because they record nothing.
    // `expectedGeneration` (#90): see uploadVideoFrameToSlot above — same
    // stale-slot reject semantics on the GPU-direct copy path.
    virtual bool       copyUploadBufferToVideoTextureSlot(
                           uint32_t /*slot*/,
                           struct UploadHeapBuffer* /*buf*/,
                           uint32_t /*width*/,
                           uint32_t /*height*/,
                           TextureFormat /*format*/,
                           uint32_t /*expectedGeneration*/ = UINT32_MAX) { return false; }
    virtual TextureRef getVideoTexture(uint32_t slot) const = 0;

    // ------------------------------------------------------------------------
    // Compose targets (offscreen per-screen composition)
    // ------------------------------------------------------------------------
    virtual uint32_t   createComposeTarget(uint32_t width, uint32_t height) = 0;

    /**
     * Resize an existing compose target in place. Frees the underlying GPU
     * texture at `slot` and creates a new one with the new dimensions,
     * reusing the same RTV + SRV descriptor heap entries so cached
     * `TextureRef`s remain valid. Returns false if the slot is invalid,
     * the new dimensions are zero, or the resize was deferred/failed (#75)
     * — callers must retry every tick until it succeeds (show thread only).
     *
     * Use this instead of allocating a new slot when a screen's resolution
     * changes (#31). Allocating new slots on every resize leaks descriptor
     * heap entries and eventually collides with adjacent slot ranges.
     */
    virtual bool       resizeComposeTarget(uint32_t slot,
                                            uint32_t width,
                                            uint32_t height) = 0;

    virtual void       beginComposeTarget(uint32_t slot = 0) = 0;
    virtual void       endComposeTarget() = 0;
    virtual TextureRef getComposeTargetTexture(uint32_t slot = 0) const = 0;
    // Editor-thread contract (#75): a compose target can be mid-resize on
    // the show thread. isComposeTargetReady / getComposeTargetTextureID are
    // gated — they return false / nullptr while a resize is pending — and a
    // window must call one of them BEFORE reading width/height in the same
    // frame (the width/height getters themselves are shared with show-side
    // callers and are not gated). StageWindow / ProjectorCalibrationWindow
    // are the reference callers.
    virtual void*      getComposeTargetTextureID(uint32_t slot = 0) const = 0;
    virtual uint32_t   getComposeTargetWidth(uint32_t slot = 0) const = 0;
    virtual uint32_t   getComposeTargetHeight(uint32_t slot = 0) const = 0;
    virtual bool       isComposeTargetReady(uint32_t slot = 0) const = 0;

    // Per-slot video-texture ImGui handle (ADR-0022 L5). Returns nullptr
    // when the slot is unallocated or has no uploaded texture. Used by
    // the ContentRoutingWindow preview to draw a clip's most-recent
    // frame under the feed-map region overlay. Same threading caveat as
    // getComposeTargetTextureID — the texture may be updated by the show
    // thread mid-frame; D3D12 common-state promotion handles the
    // graphics-queue sampling implicitly.
    virtual void*      getVideoTextureIDForSlot(uint32_t slot) const = 0;

    // ------------------------------------------------------------------------
    // Drawing primitives (transforms use glm::mat4; colors/positions use glm vec types)
    // ------------------------------------------------------------------------
    virtual void drawColoredQuad(const glm::mat4& transform,
                                  const glm::vec4& color,
                                  float opacity) = 0;

    // uvRect (ADR-0021 M3 Tiled mode) crops the source texture to a sub-region
    // before sampling. xy = uv offset, zw = uv size. Default identity
    // (0,0,1,1) samples the full source frame — bit-identical to pre-M3
    // behavior. Per-route uvRect values drive feed-map slicing where one
    // content source carves across multiple screens (`composite_vs.hlsl`
    // applies `texCoord = texCoord * uvRect.zw + uvRect.xy`).
    virtual void drawTexturedQuad(TextureRef texture,
                                   const glm::mat4& transform,
                                   float opacity,
                                   BlendMode blendMode = BlendMode::Normal,
                                   TextureColorSpace colorSpace = TextureColorSpace::Linear,
                                   const std::string& ocioColorSpace = std::string(),
                                   const glm::vec4& uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)) = 0;

    virtual void drawOutputSurface(TextureRef texture,
                                     const glm::vec2 corners[4],
                                     const glm::vec2 sourceUVs[4],
                                     const glm::vec4& softEdges,
                                     float brightness,
                                     float gamma,
                                     float opacity) = 0;

    // Draws one textured triangle (3 NDC verts + 3 UVs) with proper hardware
    // barycentric UV interpolation. Use for mesh rendering where bilinear
    // approximation in drawOutputSurface causes texture seams. Must be called
    // between beginOutputFrame / endOutputFrame.
    virtual void drawMeshTriangle(TextureRef texture,
                                   const glm::vec2 verts[3],
                                   const glm::vec2 uvs[3]) = 0;

    // ------------------------------------------------------------------------
    // Per-layer effect chain dispatch (issue #54, PASS 1.5 of CompositorSystem).
    //
    // Draws a single fullscreen-triangle effect pass that samples `input` and
    // writes to the currently-bound compose target. `kindIdHash` selects the
    // shader (FNV-1a of the effect's stable string ID; the renderer keeps a
    // PSO cache populated lazily from an EffectKindRegistry pointer set at
    // startup). `paramBlob` is the 256-byte cbuffer-layout marshalled by the
    // editor-side bake (PlaybackTimeAuthority::buildSceneSnapshot); the
    // renderer copies it verbatim into a per-frame upload-heap ring slot bound
    // at root parameter 1 (b0). Must be called between beginComposeTarget and
    // endComposeTarget. ------------------------------------------------------------------------
    virtual void drawEffectPass(TextureRef input,
                                std::uint32_t kindIdHash,
                                const std::uint8_t* paramBlob,
                                std::size_t paramBlobSize,
                                std::uint32_t viewportWidth,
                                std::uint32_t viewportHeight) = 0;

    // Draws the projector calibration crosshair overlay directly onto the
    // currently-bound output RTV. Show-thread call (ADR-0014); records on the
    // show command list. pointsXY is a flat array of [x0,y0,x1,y1,...] of
    // length numPoints*2 (numPoints clamped to 16). Must be called between
    // beginOutputFrame / endOutputFrame.
    virtual void drawCalibrationOverlay(const float* pointsXY, int numPoints,
                                         int activeIndex, bool precisionCursor) = 0;

    // ------------------------------------------------------------------------
    // Output windows (physical display outputs)
    //
    // Create a borderless OS-level window (backed by its own DXGI swap chain)
    // positioned on a specific physical display. The renderer owns the window;
    // destroyOutputWindow is the only way to close it.
    //
    // Per-frame flow:
    //   beginShowFrame()
    //     ... compose-target rendering ...
    //     for each output: beginOutputFrame(slot) -> draws -> endOutputFrame(slot)
    //   endShowFrame()   // presents output swap chains
    //   beginEditorFrame()
    //     ... ImGui overlay on main window ...
    //   endEditorFrame()   // presents main swap chain
    // ------------------------------------------------------------------------

    /** Create a borderless output window at desktop (x,y) with size (w,h).
     *  Returns a slot ID to pass to subsequent output methods, or UINT32_MAX
     *  on failure. The window is unfocused, un-decorated, and ignores input —
     *  it's a projection surface, not an editor. */
    virtual uint32_t createOutputWindow(const char* title,
                                         int32_t x, int32_t y,
                                         uint32_t width, uint32_t height) = 0;

    virtual void     destroyOutputWindow(uint32_t outputSlot) = 0;
    virtual void     resizeOutputWindow(uint32_t outputSlot,
                                         uint32_t width, uint32_t height) = 0;
    virtual uint32_t getOutputWindowWidth(uint32_t outputSlot) const = 0;
    virtual uint32_t getOutputWindowHeight(uint32_t outputSlot) const = 0;

    /** Switch the active RT to this output's back buffer (transitions to
     *  RENDER_TARGET, sets viewport/scissor to output size). Subsequent draw
     *  calls target the output. */
    virtual void beginOutputFrame(uint32_t outputSlot) = 0;

    /** Clear the active output back buffer (must be inside begin/endOutputFrame). */
    virtual void clearOutputFrame(uint32_t outputSlot,
                                   float r, float g, float b, float a) = 0;

    /** Transition output back buffer back to PRESENT state and restore the
     *  main render target + viewport so ImGui / further draws target the main
     *  window. */
    virtual void endOutputFrame(uint32_t outputSlot) = 0;

    // ------------------------------------------------------------------------
    // Screenshot / pixel readback
    // ------------------------------------------------------------------------
    virtual bool captureComposeTargetToPNG(const std::string& filepath, uint32_t slot = 0) = 0;
    virtual bool captureBackBufferToPNG(const std::string& filepath) = 0;
    virtual bool readComposeTargetPixels(uint32_t slot,
                                          uint32_t& outWidth,
                                          uint32_t& outHeight,
                                          std::vector<uint8_t>& outPixels) = 0;
};

} // namespace entity
