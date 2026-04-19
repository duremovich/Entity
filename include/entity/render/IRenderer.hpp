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
    static constexpr uint32_t MAX_COMPOSE_TARGETS           = 8;
    static constexpr uint32_t MAX_MAPPING_SURFACES_PER_FRAME = 64;

    virtual ~IRenderer() = default;

    // ------------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------------
    virtual Result initialize(GLFWwindow* window, uint32_t width, uint32_t height) = 0;
    virtual void   shutdown() = 0;
    virtual Result resize(uint32_t width, uint32_t height) = 0;
    virtual bool   isInitialized() const = 0;
    virtual bool   isDeviceLost() const = 0;

    // ------------------------------------------------------------------------
    // Per-frame
    // ------------------------------------------------------------------------
    virtual void     beginFrame() = 0;
    virtual void     endFrame() = 0;
    virtual void     clear(float r, float g, float b, float a) = 0;
    virtual uint32_t getCurrentBackBufferIndex() const = 0;
    virtual void     beginImGuiFrame() = 0;
    virtual void     endImGuiFrame() = 0;

    // ------------------------------------------------------------------------
    // Video texture slots (per-clip video uploads)
    // ------------------------------------------------------------------------
    virtual uint32_t   allocateVideoTextureSlot() = 0;
    virtual void       freeVideoTextureSlot(uint32_t slot) = 0;
    virtual bool       uploadVideoFrameToSlot(uint32_t slot,
                                              const uint8_t* rgba,
                                              uint32_t width,
                                              uint32_t height) = 0;
    virtual TextureRef getVideoTexture(uint32_t slot) const = 0;

    // ------------------------------------------------------------------------
    // Compose targets (offscreen per-screen composition)
    // ------------------------------------------------------------------------
    virtual uint32_t   createComposeTarget(uint32_t width, uint32_t height) = 0;
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
                                   BlendMode blendMode = BlendMode::Normal) = 0;

    virtual void drawMappingSurface(TextureRef texture,
                                     const glm::vec2 corners[4],
                                     const glm::vec2 sourceUVs[4],
                                     const glm::vec4& softEdges,
                                     float brightness,
                                     float gamma,
                                     float opacity) = 0;

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
