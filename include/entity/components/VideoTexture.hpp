#pragma once

#include "../core/Types.hpp"
#include <cstdint>

#ifdef _WIN32
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// Forward declarations for D3D12 types
struct ID3D12Resource;
struct D3D12_GPU_DESCRIPTOR_HANDLE;
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
#ifdef _WIN32
    ComPtr<ID3D12Resource> gpuTexture;      // Texture on DEFAULT heap (GPU)
    ComPtr<ID3D12Resource> uploadBuffer;    // Staging buffer on UPLOAD heap
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{};  // Shader Resource View handle
#endif

    uint32_t width{0};
    uint32_t height{0};
    PixelFormat format{PixelFormat::RGBA8};

    bool needsUpload{false};                // True if new frame ready for upload
    AVFrame* currentFrame{nullptr};         // Frame to upload (owned by FrameBuffer)

    // Synchronization
    uint64_t fenceValue{0};                 // Fence value for upload completion

    /**
     * Check if texture is valid and ready for rendering.
     */
    bool isValid() const {
#ifdef _WIN32
        return gpuTexture.Get() != nullptr && width > 0 && height > 0;
#else
        return false;
#endif
    }
};

} // namespace entity
