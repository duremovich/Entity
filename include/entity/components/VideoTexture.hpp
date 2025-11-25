#pragma once

#include "../core/Types.hpp"
#include <cstdint>

#ifdef _WIN32
#include <d3d12.h>  // For D3D12_GPU_DESCRIPTOR_HANDLE
#endif

// Forward declaration for FFmpeg
struct AVFrame;

namespace entity {

/**
 * VideoTexture component for GPU texture resources.
 *
 * Manages D3D12 texture resources, upload buffers, and shader resource views.
 * One texture per video layer.
 */
struct VideoTexture {
    // Slot index in D3D12Renderer's texture slot array (UINT32_MAX = not allocated)
    uint32_t descriptorSlot{UINT32_MAX};

    // Texture dimensions (set after upload)
    uint32_t width{0};
    uint32_t height{0};
    PixelFormat format{PixelFormat::RGBA8};

    // Frame data to upload (owned by decode system, not this component)
    bool needsUpload{false};                // True if new frame ready for upload
    AVFrame* currentFrame{nullptr};         // Frame to upload (owned by FrameBuffer)

    // Synchronization
    uint64_t fenceValue{0};                 // Fence value for upload completion

#ifdef _WIN32
    // GPU descriptor handle for rendering (populated by renderer on upload)
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{};
#endif

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
