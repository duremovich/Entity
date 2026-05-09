#pragma once

/**
 * MeshUploader - slot pool for vertex/index buffer GPU uploads.
 *
 * Unlike TextureUploader, frees are deferred via a fence-keyed pending list
 * since Models can be destroyed mid-frame (before the GPU finishes reading).
 *
 * Cross-queue fence sync (copy → direct) is owned by D3D12Renderer, not here.
 * MeshUploader requires onFrameBegin(completedFenceValue) to be called each
 * frame (from beginEditorFrame) so pending frees can be reaped safely.
 */

#include "entity/media/ObjLoader.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace entity {

using Microsoft::WRL::ComPtr;

class MeshUploader {
public:
    static constexpr uint32_t MAX_MESH_SLOTS = 32;
    static constexpr uint32_t INVALID_SLOT   = UINT32_MAX;

    MeshUploader() = default;
    ~MeshUploader() = default;

    MeshUploader(const MeshUploader&)            = delete;
    MeshUploader& operator=(const MeshUploader&) = delete;

    /**
     * Initialize with the D3D12 device. Must be called before any upload.
     */
    bool initialize(ID3D12Device* device);

    /**
     * Release all GPU resources. Safe to call multiple times.
     * Caller must have called waitForGpu() before invoking this.
     */
    void shutdown();

    /**
     * Upload vertex + index data into a new slot pair. The command list must
     * live on the copy queue; caller executes + signals the fence.
     *
     * Returns the slot index on success (both vertex and index share one slot).
     * Returns INVALID_SLOT if the pool is exhausted or any allocation fails.
     */
    uint32_t uploadMesh(ID3D12GraphicsCommandList* cmdList,
                        const std::vector<MeshVertex>& vertices,
                        const std::vector<uint32_t>& indices);

    /**
     * Schedule a slot for deferred release keyed on the editor fence.
     * Pass the editor fence's next-to-be-signaled value (m_editorFenceValue + 1
     * before Signal, or the value D3D12Renderer will Signal at endEditorFrame).
     * The slot is not freed until onFrameBegin sees the editor fence completed
     * value pass this number, guaranteeing both the copy-queue upload AND the
     * direct-queue DrawIndexedInstanced that read the buffers are finished.
     */
    void scheduleSlotFree(uint32_t slot, uint64_t editorFenceValueToWaitFor);

    /**
     * Called from D3D12Renderer::beginEditorFrame with the editor fence's
     * completed value (m_editorFence->GetCompletedValue()). Reaps any pending
     * frees whose recorded fence value has been passed by the GPU.
     */
    void onFrameBegin(uint64_t completedEditorFenceValue);

    struct BindHandle {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        D3D12_INDEX_BUFFER_VIEW  ibv;
        uint32_t indexCount;
    };

    /**
     * Fill out a BindHandle for the given slot. Returns false if the slot is
     * not valid / not yet uploaded.
     */
    bool getBindHandle(uint32_t slot, BindHandle& out) const;

    // Total successful uploads since construction. Used by integration tests
    // to verify upload pacing (load-bearing for mesh_upload_paces.json).
    uint64_t uploadCount() const { return m_totalUploadCount.load(std::memory_order_relaxed); }

private:
    struct Slot {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        ComPtr<ID3D12Resource> vertexUpload;
        ComPtr<ID3D12Resource> indexUpload;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW  ibv{};
        uint32_t indexCount{0};
        bool allocated{false};
    };

    struct PendingFree {
        uint32_t slot;
        uint64_t fenceValue;
    };

    void actuallyFreeSlot(uint32_t slot);

    ID3D12Device*      m_device{nullptr};
    Slot               m_slots[MAX_MESH_SLOTS];
    mutable std::mutex m_slotMutex;
    std::atomic<uint64_t> m_totalUploadCount{0};

    std::vector<PendingFree> m_pendingFrees;
    std::mutex               m_pendingMutex;
};

} // namespace entity
