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
#include <mutex>

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
     * Proactively create the GPU texture + upload buffer + SRV descriptor for
     * `slot` without copying any pixels. Use this from the editor thread at
     * project load (when the source dimensions are known) to avoid paying the
     * two CreateCommittedResource calls (texture + upload buffer, ~33MB each
     * for 4K) on the show thread at the first uploadVideoFrameToSlot call.
     *
     * That first lazy creation was the cause of the section-break stutter
     * (2026-05-23): a queued-at-break clip's slot was allocated at project
     * load but the underlying D3D12 resources weren't created until the
     * playhead reached the break and PlaybackPresenter::present made the
     * first upload — blocking the show frame for ~30-100ms.
     *
     * Idempotent: if the slot is already prepared with matching dimensions
     * and format, this is a no-op. Mismatched dimensions trigger the same
     * release+recreate that ensureTexture does for upload().
     *
     * **Threading contract:** safe to call from any thread when the slot is
     * not concurrently being uploaded to. The intended use is at project
     * load (editor thread, before the show thread first presents this slot),
     * which satisfies that invariant by construction. The internal
     * m_slotMutex serializes concurrent prepareTexture() calls but does NOT
     * race-protect against upload() — that path doesn't lock. If we ever
     * need mid-session prepare for an in-use slot, add waitForGpu + lock
     * around upload's ensureTexture call first.
     *
     * Returns false on slot-out-of-bounds, unallocated slot, or D3D12
     * resource-creation failure.
     */
    bool prepareTexture(uint32_t slot,
                        uint32_t width,
                        uint32_t height,
                        TextureFormat format = TextureFormat::RGBA8_UNORM);

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
    mutable std::mutex     m_slotMutex;
};

} // namespace entity
