#pragma once

/**
 * TextureUploader - owns the pool of GPU video textures and the upload
 * pipeline (staging buffer → GPU texture) used by D3D12Renderer.
 *
 * Previously this was ~200 lines of slot-array management + per-upload
 * bookkeeping inlined into D3D12Renderer. Extracted to a standalone class
 * so the renderer stays focused on composition; also makes the upload path
 * easier to reason about when the CRIT-04-style resource lifetimes matter.
 *
 * This class is D3D12-specific and is only used by D3D12Renderer. When a
 * Metal backend lands it will have its own MetalTextureUploader equivalent.
 */

#include "entity/core/Types.hpp"
#include "entity/render/DescriptorHeapLayout.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace entity {

using Microsoft::WRL::ComPtr;

class TextureUploader {
public:
    static constexpr uint32_t MAX_SLOTS = DescriptorHeapLayout::MAX_VIDEO_TEXTURE_SLOTS;
    static constexpr uint32_t INVALID_SLOT = UINT32_MAX;

    TextureUploader() = default;
    ~TextureUploader() = default;

    // Non-copyable, non-movable (holds GPU resources with strict lifetime)
    TextureUploader(const TextureUploader&) = delete;
    TextureUploader& operator=(const TextureUploader&) = delete;

    /**
     * Initialize with the device + shader-visible SRV heap. The uploader
     * writes SRV descriptors into the heap at slots computed via
     * DescriptorHeapLayout::videoTextureSlot().
     */
    Result initialize(ID3D12Device* device,
                      ID3D12DescriptorHeap* srvHeap,
                      uint32_t srvDescriptorSize);

    /**
     * Release all GPU resources. Safe to call multiple times. Must be called
     * before the backing device is released.
     */
    void shutdown();

    /**
     * Reserve a free slot. Returns INVALID_SLOT if the pool is exhausted.
     */
    uint32_t allocateSlot();

    /**
     * Release a previously allocated slot and free its GPU resources.
     */
    void freeSlot(uint32_t slot);

    /**
     * True if the slot has been allocated (but maybe not yet uploaded).
     */
    bool isAllocated(uint32_t slot) const;

    /**
     * True if an upload has succeeded for this slot. IRenderer uses this
     * to decide whether getVideoTexture(slot) should yield a valid ref.
     */
    bool hasTexture(uint32_t slot) const;

    /**
     * GPU descriptor handle for sampling this slot's texture. Only valid
     * if hasTexture(slot). Returns {.ptr=0} otherwise.
     */
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(uint32_t slot) const;

    /**
     * Returns true if a subsequent upload(slot, w, h, fmt, ...) would resize or
     * re-create the texture (first upload, dimension change, or format change).
     * Caller should waitForGpu() before calling upload() in that case.
     */
    bool uploadWouldResize(uint32_t slot, uint32_t width, uint32_t height,
                           TextureFormat format = TextureFormat::RGBA8_UNORM) const;

    /**
     * Perform an upload. Records copy commands into `cmdList`:
     *  1. If first upload, dimensions changed, or format changed, (re)creates
     *     the GPU texture, upload buffer, and SRV descriptor using the DXGI
     *     format that matches `format`.
     *  2. memcpy source data into the upload buffer (with row-pitch fix-up
     *     that correctly handles BC-block row pitch for compressed formats).
     *  3. Records: transition COPY_DEST, CopyTextureRegion, transition
     *     PIXEL_SHADER_RESOURCE.
     * Caller is responsible for executing the command list and fence-signaling.
     *
     * `data` is RGBA for RGBA8_UNORM, or densely-packed BC blocks for BC*.
     * Format defaults to RGBA8_UNORM so legacy callers compile unchanged.
     *
     * Returns false on slot-out-of-bounds, uninitialized, bad args, or
     * resource-creation failure.
     */
    bool upload(ID3D12GraphicsCommandList* cmdList,
                uint32_t slot,
                const uint8_t* data,
                uint32_t width,
                uint32_t height,
                TextureFormat format = TextureFormat::RGBA8_UNORM);

private:
    struct Slot {
        ComPtr<ID3D12Resource> texture;        // Resting state: D3D12_RESOURCE_STATE_COMMON (Phase C.11)
        ComPtr<ID3D12Resource> uploadBuffer;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
        uint32_t width{0};
        uint32_t height{0};
        TextureFormat format{TextureFormat::RGBA8_UNORM};
        bool allocated{false};
    };

    bool ensureTexture(Slot& slot, uint32_t slotIndex, uint32_t width, uint32_t height,
                       TextureFormat format);
    bool copyPixelsAndRecord(ID3D12GraphicsCommandList* cmdList,
                              Slot& slot,
                              const uint8_t* data,
                              uint32_t width,
                              uint32_t height,
                              TextureFormat format);

    ID3D12Device*          m_device{nullptr};
    ID3D12DescriptorHeap*  m_srvHeap{nullptr};
    uint32_t               m_srvDescriptorSize{0};
    Slot                   m_slots[MAX_SLOTS];
};

} // namespace entity
