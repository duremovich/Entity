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

    bool valid() const { return kind != Kind::Invalid; }

    static TextureRef invalid()            { return {}; }
    static TextureRef video(uint32_t s)    { return {Kind::VideoSlot, s}; }
    static TextureRef compose(uint32_t s)  { return {Kind::ComposeTarget, s}; }
};

class IRenderer {
public:
    // Limits callers need to know about. Must match backend implementation.
    static constexpr uint32_t MAX_VIDEO_TEXTURE_SLOTS       = 16;
    // Compose-target pool — shared between screens and generative-layer RTs
    // (ADR-0018). 32 is comfortable for typical shows; raise (or split into
    // per-kind pools) when an installation pushes total active screens +
    // generative layers past this.
    static constexpr uint32_t MAX_COMPOSE_TARGETS           = 32;
    // 8192 leaves room for projector-output mesh rendering with per-triangle
    // barycentric subdivision in OutputManager (each mesh triangle becomes N×N
    // sub-triangles, each a drawMappingSurface call). 256-byte aligned slots ×
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
    virtual void       freeVideoTextureSlot(uint32_t slot) = 0;
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
    virtual bool       uploadVideoFrameToSlot(uint32_t slot,
                                              const uint8_t* data,
                                              uint32_t width,
                                              uint32_t height,
                                              TextureFormat format) = 0;
    virtual TextureRef getVideoTexture(uint32_t slot) const = 0;

    // ------------------------------------------------------------------------
    // Compose targets (offscreen per-screen composition)
    // ------------------------------------------------------------------------
    virtual uint32_t   createComposeTarget(uint32_t width, uint32_t height) = 0;

    /**
     * Resize an existing compose target in place. Frees the underlying GPU
     * texture at `slot` and creates a new one with the new dimensions,
     * reusing the same RTV + SRV descriptor heap entries so cached
     * `TextureRef`s remain valid. Returns false if the slot is invalid
     * or the new dimensions are zero.
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
    virtual void*      getComposeTargetTextureID(uint32_t slot = 0) const = 0;
    virtual uint32_t   getComposeTargetWidth(uint32_t slot = 0) const = 0;
    virtual uint32_t   getComposeTargetHeight(uint32_t slot = 0) const = 0;
    virtual bool       isComposeTargetReady(uint32_t slot = 0) const = 0;

    // ------------------------------------------------------------------------
    // Drawing primitives (transforms use glm::mat4; colors/positions use glm vec types)
    // ------------------------------------------------------------------------
    virtual void drawColoredQuad(const glm::mat4& transform,
                                  const glm::vec4& color,
                                  float opacity) = 0;

    virtual void drawTexturedQuad(TextureRef texture,
                                   const glm::mat4& transform,
                                   float opacity,
                                   BlendMode blendMode = BlendMode::Normal,
                                   TextureColorSpace colorSpace = TextureColorSpace::Linear,
                                   const std::string& ocioColorSpace = std::string()) = 0;

    virtual void drawMappingSurface(TextureRef texture,
                                     const glm::vec2 corners[4],
                                     const glm::vec2 sourceUVs[4],
                                     const glm::vec4& softEdges,
                                     float brightness,
                                     float gamma,
                                     float opacity) = 0;

    // Draws one textured triangle (3 NDC verts + 3 UVs) with proper hardware
    // barycentric UV interpolation. Use for mesh rendering where bilinear
    // approximation in drawMappingSurface causes texture seams. Must be called
    // between beginOutputFrame / endOutputFrame.
    virtual void drawMeshTriangle(TextureRef texture,
                                   const glm::vec2 verts[3],
                                   const glm::vec2 uvs[3]) = 0;

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
