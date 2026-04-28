#pragma once

#include "../core/Types.hpp"
#include <cstdint>
#include <string>

// Forward declaration for FFmpeg
struct AVFrame;

namespace entity {

/**
 * VideoTexture component — backend-agnostic GPU texture reference for a clip.
 *
 * The slot index `descriptorSlot` is the only handle callers need; the
 * renderer resolves it to a backend-specific descriptor (D3D12 SRV handle,
 * Metal argument buffer slot, etc.) via IRenderer::getVideoTexture(slot).
 */
struct VideoTexture {
    // Slot index into the renderer's video texture pool (UINT32_MAX = not allocated).
    // Allocate via IRenderer::allocateVideoTextureSlot().
    uint32_t descriptorSlot{UINT32_MAX};

    // Texture dimensions (set after upload)
    uint32_t width{0};
    uint32_t height{0};
    PixelFormat format{PixelFormat::RGBA8};

    // Sampler-side colour-space hint for the compositor PS. HAP Q content
    // sets this to YCoCg_scaled; everything else stays Linear. Sticky once
    // set — colour space is a property of the codec, not of any one frame —
    // but PlaybackController stamps it on every upload so a clip swap (e.g.
    // re-import as a different variant) is picked up without extra wiring.
    TextureColorSpace colorSpace{TextureColorSpace::Linear};

    // Phase C.12 #6 — OCIO color-space name for the input transform. Mirrors
    // the per-frame DecodedFrame.ocioColorSpace; PlaybackController stamps
    // this on every upload so the compositor can look up the right OCIO
    // input processor at draw time. Empty = identity (no transform applied).
    std::string ocioColorSpace;

    // Frame data to upload (owned by decode system, not this component)
    bool needsUpload{false};                // True if new frame ready for upload
    AVFrame* currentFrame{nullptr};         // Frame to upload (owned by FrameBuffer)

    // Synchronization
    uint64_t fenceValue{0};                 // Fence value for upload completion

    /**
     * Check if texture slot is allocated and ready for use.
     */
    bool isAllocated() const {
        return descriptorSlot != UINT32_MAX;
    }

    /**
     * Check if texture has been uploaded and is ready for rendering.
     */
    bool isValid() const {
        return isAllocated() && width > 0 && height > 0;
    }
};

} // namespace entity
