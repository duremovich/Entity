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
#include <atomic>
#include <cstdint>
#include <mutex>

namespace entity {

using Microsoft::WRL::ComPtr;

class TextureUploader {
public:
    static constexpr uint32_t MAX_SLOTS = DescriptorHeapLayout::MAX_VIDEO_TEXTURE_SLOTS;
    static constexpr uint32_t INVALID_SLOT = UINT32_MAX;
    // Passed as expectedGeneration to skip the per-slot generation check (the
    // default for legacy/text/compose callers that never reuse a slot index
    // across a stale cached snapshot). See slotGeneration() / #90.
    static constexpr uint32_t kSkipGenerationCheck = UINT32_MAX;

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
     * Returns the count of currently allocated slots. Used by
     * AssertVideoTextureSlotsAllocated integration tests.
     */
    uint32_t allocatedSlotCount() const;

    /**
     * True if an upload has succeeded for this slot. IRenderer uses this
     * to decide whether getVideoTexture(slot) should yield a valid ref.
     */
    bool hasTexture(uint32_t slot) const;

    /**
     * Per-slot reuse generation (#90). Starts at 0; bumped once per free→
     * realloc cycle inside freeSlot (the first occupant of a fresh index sees
     * gen 0). Callers capture the generation at the time they resolve a slot
     * (provision / bake) and pass it back as expectedGeneration; a mismatch at
     * upload()/recordDirectCopy() means the index was freed and handed to a
     * different clip since, so the upload is safely skipped.
     *
     * Invariant: the only store is in freeSlot under m_slotMutex; this read is
     * lock-free (relaxed atomic load) because it runs on the show-thread hot
     * path (resolveTextureHandle). Readers tolerate staleness — the worst case
     * is one safely-skipped upload that the presenter re-issues next tick.
     * Returns 0 for an out-of-bounds slot.
     */
    uint32_t slotGeneration(uint32_t slot) const;

    /**
     * Count of uploads rejected because expectedGeneration did not match the
     * slot's current generation (#90). Monotonic; read by the
     * AssertVideoTextureStaleGenerationSkips integration command to prove the
     * cross-tick stale-snapshot guard actually fired.
     */
    uint64_t staleGenerationSkips() const {
        return m_staleGenerationSkips.load(std::memory_order_relaxed);
    }

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
     * Proactively create the GPU (DEFAULT-heap) texture + SRV descriptor for
     * `slot` without copying any pixels. Use this from the editor thread at
     * project load (when the source dimensions are known) to avoid paying the
     * CreateCommittedResource call (~33MB for 4K) on the show thread at the
     * first upload call.
     *
     * That first lazy creation was the cause of the section-break stutter
     * (2026-05-23): a queued-at-break clip's slot was allocated at project
     * load but the underlying D3D12 texture wasn't created until the
     * playhead reached the break and PlaybackPresenter::present made the
     * first upload — blocking the show frame for ~30-100ms.
     *
     * Idempotent: if the slot is already prepared with matching dimensions
     * and format, this is a no-op. Mismatched dimensions trigger the same
     * release+recreate that ensureTexture does for upload().
     *
     * **Threading contract:** safe to call from any thread. The intended use
     * is at project load (editor thread, before the show thread first presents
     * this slot). The internal m_slotMutex serializes concurrent
     * prepareTexture() calls against each other AND against upload() /
     * recordDirectCopy() (#90 — those paths now take m_slotMutex around their
     * whole body, so an editor prepareTexture that recreates a slot's ComPtrs
     * can no longer race a show-thread upload reading them). The stale-cached-
     * snapshot hazard (editor frees + reallocates a slot index while the show
     * thread holds a snapshot mapping the old clip to it) is defended
     * separately by the per-slot generation guard — see slotGeneration().
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
     *     the GPU texture, upload-buffer, and SRV descriptor using the DXGI
     *     format that matches `format`.
     *  2. Memcpys source data into the upload buffer (with row-pitch fix-up
     *     for BC-block formats) and records CopyTextureRegion.
     *  3. No explicit barriers (Phase C.11 implicit-state promotion applies).
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
                TextureFormat format = TextureFormat::RGBA8_UNORM,
                uint32_t expectedGeneration = kSkipGenerationCheck);


    /**
     * Direct-copy variant for upload-heap-backed FrameCache frames (Phase 5).
     *
     * Records a CopyTextureRegion from `uploadResource` (an UPLOAD-heap resource
     * already written by a decode worker) into the slot's DEFAULT-heap texture
     * without any CPU memcpy. `uploadResource` must be the persistently-mapped
     * resource from UploadHeapBuffer and must have been written with the correct
     * pitch alignment (acquireForTexture / acquireForTexture-derived footprint).
     *
     * Calls ensureTexture so the first call for a new slot still creates the
     * GPU texture + SRV. Falls back to a no-op (returns false) if the slot
     * is invalid or the GPU texture hasn't been created yet (uploadWouldResize
     * returns true AND hasTexture returns false — caller should call upload()
     * or prepareTexture() first for that slot).
     *
     * The footprint must be the one returned by
     * UploadHeapBufferPool::acquireForTexture (stored in UploadHeapBuffer::footprint).
     * Passing the pre-computed footprint avoids a redundant GetCopyableFootprints
     * call on the show-thread hot path.
     *
     * Returns true on success.
     */
    bool recordDirectCopy(ID3D12GraphicsCommandList* cmdList,
                          uint32_t slot,
                          ID3D12Resource* uploadResource,
                          const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                          uint32_t width,
                          uint32_t height,
                          TextureFormat format = TextureFormat::RGBA8_UNORM,
                          uint32_t expectedGeneration = kSkipGenerationCheck);

private:
    struct Slot {
        ComPtr<ID3D12Resource> texture;        // Resting state: D3D12_RESOURCE_STATE_COMMON (Phase C.11)
        ComPtr<ID3D12Resource> uploadBuffer;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
        uint32_t width{0};
        uint32_t height{0};
        TextureFormat format{TextureFormat::RGBA8_UNORM};
        bool allocated{false};
        // #90 reuse counter. Bumped in freeSlot under m_slotMutex; read lock-
        // free (relaxed) via slotGeneration() from the show thread. Atomic so
        // the cross-thread read in resolveTextureHandle and the Phase-1 stress
        // test aren't UB. Slot lives only in the fixed m_slots array and is
        // never copied/moved, so no move-ctor is needed for the atomic member.
        std::atomic<uint32_t> generation{0};
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
    // #90: count of uploads rejected on a generation mismatch. No per-frame
    // logging — polled by AssertVideoTextureStaleGenerationSkips.
    std::atomic<uint64_t>  m_staleGenerationSkips{0};
};

} // namespace entity
